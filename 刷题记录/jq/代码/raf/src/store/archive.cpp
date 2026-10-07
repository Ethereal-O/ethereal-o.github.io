#include "store/archive.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <system_error>

#include "common/log.h"

namespace raf::store {
namespace {

constexpr std::string_view kPrefix = "seg-";
constexpr std::string_view kSuffix = ".raf";
constexpr int kDigits = 5;

std::uint64_t nowNanos() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

// Opens segment `id`, creating it if needed, and claims it. Returns null when
// the segment belongs to someone else, is not a segment we understand, or has
// no room left.
std::unique_ptr<Segment> claim(io::IoBackend& backend, const std::string& dir, std::uint32_t id,
                               const Options& options, std::uint64_t min_free) {
    const std::string path = Archive::segmentPath(dir, id);
    io::OpenFlags flags;
    flags.create = true;
    flags.sync = options.sync;

    const int fd = backend.open(path, flags);
    bool created = false;
    std::unique_ptr<Segment> segment;
    try {
        if (!backend.tryLockExclusive(fd)) {
            backend.close(fd);
            return nullptr;
        }
        created = backend.size(fd) == 0;
        segment = std::make_unique<Segment>(backend, fd, id, path, /*locked=*/true);
    } catch (...) {
        backend.close(fd);
        throw;
    }

    if (created) {
        // A fresh file only counts once its directory entry is durable too,
        // otherwise a crash could leave records that no longer have a name.
        segment->initialize(nowNanos(), options.maxRecordBytes);
        backend.syncDirectory(dir);
        segment->configurePreallocation(options.preallocateBytes);
        RAF_LOG_INFO("created segment {}", path);
        return segment;
    }

    if (!segment->verifyHeader()) {
        RAF_LOG_WARN("ignoring {}: not a readable raf segment", path);
        return nullptr;
    }

    // Before recover(), so it knows the zero tail is deliberate and leaves
    // it where it is.
    segment->configurePreallocation(options.preallocateBytes);
    const ScanResult scan = segment->recover();
    segment->configurePreallocation(options.preallocateBytes);
    if (scan.truncated_bytes != 0 || scan.damaged_records != 0) {
        RAF_LOG_WARN("recovered {}: {} records, {} damaged, dropped {} trailing bytes", path,
                     scan.record_count, scan.damaged_records, scan.truncated_bytes);
    } else {
        RAF_LOG_DEBUG("adopted {}: {} records, next offset {}", path, scan.record_count,
                      scan.end_offset);
    }

    // An empty segment is taken whatever it is asked for: something has to
    // accept a record larger than maxSegmentBytes, and a fresh file is the
    // only place it can go.
    if (segment->writeOffset() > format::kHeaderSize &&
        segment->writeOffset() + min_free > options.maxSegmentBytes) {
        return nullptr;  // no room; leave it for readers and take another
    }
    return segment;
}

}  // namespace

std::string Archive::segmentPath(const std::string& dir, std::uint32_t id) {
    char name[32];
    std::snprintf(name, sizeof(name), "%.*s%0*u%.*s", static_cast<int>(kPrefix.size()),
                  kPrefix.data(), kDigits, id, static_cast<int>(kSuffix.size()), kSuffix.data());
    std::string path = dir;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path.append(name);
    return path;
}

std::optional<std::uint32_t> Archive::parseSegmentName(std::string_view name) {
    if (name.size() <= kPrefix.size() + kSuffix.size()) {
        return std::nullopt;
    }
    if (name.substr(0, kPrefix.size()) != kPrefix ||
        name.substr(name.size() - kSuffix.size()) != kSuffix) {
        return std::nullopt;
    }
    const std::string_view digits =
        name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
    std::uint32_t id = 0;
    const auto* end = digits.data() + digits.size();
    const auto parsed = std::from_chars(digits.data(), end, id);
    if (parsed.ec != std::errc{} || parsed.ptr != end || id >= kMaxSegments) {
        return std::nullopt;
    }
    return id;
}

std::vector<std::uint32_t> Archive::listSegments(io::IoBackend& backend, const std::string& dir) {
    std::vector<std::uint32_t> ids;
    for (const std::string& name : backend.listDirectory(dir)) {
        if (const std::optional<std::uint32_t> id = parseSegmentName(name)) {
            ids.push_back(*id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

void Archive::create(io::IoBackend& backend, const std::string& dir) {
    if (backend.directoryExists(dir)) {
        return;
    }
    backend.makeDirectory(dir);
    if (!backend.directoryExists(dir)) {
        // mkdir reported EEXIST but there is no directory there, so the path
        // names something else -- a regular file, most likely.
        throw std::system_error(ENOTDIR, std::system_category(), "open archive " + dir);
    }
    RAF_LOG_INFO("created archive {}", dir);
}

void Archive::requireExists(io::IoBackend& backend, const std::string& dir) {
    if (!backend.directoryExists(dir)) {
        throw std::system_error(ENOENT, std::system_category(), "open archive " + dir);
    }
}

std::unique_ptr<Segment> Archive::acquireForWrite(io::IoBackend& backend, const std::string& dir,
                                                  const Options& options, std::uint64_t min_free) {
    const std::vector<std::uint32_t> existing = listSegments(backend, dir);
    for (const std::uint32_t id : existing) {
        if (std::unique_ptr<Segment> segment = claim(backend, dir, id, options, min_free)) {
            return segment;
        }
    }

    // Everything on disk is taken or full. Racing writers can pick the same
    // next id; the one that loses the lock simply moves on to the one after.
    std::uint32_t next = existing.empty() ? 0 : existing.back() + 1;
    for (; next < kMaxSegments; ++next) {
        if (std::unique_ptr<Segment> segment = claim(backend, dir, next, options, min_free)) {
            return segment;
        }
    }
    throw std::logic_error("raf: archive " + dir + " has no segment left to write to");
}

std::unique_ptr<Segment> Archive::openForRead(io::IoBackend& backend, const std::string& dir,
                                              std::uint32_t id, bool direct) {
    const std::string path = segmentPath(dir, id);
    io::OpenFlags flags;
    flags.read_only = true;
    flags.direct = direct;
    int err = 0;
    const int fd = backend.tryOpen(path, flags, &err);
    if (fd < 0) {
        if (err == ENOENT) {
            return nullptr;
        }
        throw std::system_error(err, std::system_category(), "open " + path);
    }
    auto segment = std::make_unique<Segment>(backend, fd, id, path, /*locked=*/false, direct);
    if (!segment->verifyHeader()) {
        RAF_LOG_WARN("ignoring {}: not a readable raf segment", path);
        return nullptr;
    }
    return segment;
}

}  // namespace raf::store
