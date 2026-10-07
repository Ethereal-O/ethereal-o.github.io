#include "store/segment.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <vector>

#include "common/log.h"
#include "common/string_util.h"

namespace raf::store {
namespace {

using format::kHeaderSize;
using format::kMetaSize;
using format::RecordMeta;
using format::SegmentHeader;

// Ceiling on how much payload a random read will speculate on. Below this a
// wrong guess costs a re-read out of page cache; above it the record is large
// enough that the extra system call is lost in the payload anyway.
constexpr std::uint32_t kMaxSpeculation = 64u << 10;

iovec makeIov(const void* base, std::size_t length) {
    iovec v{};
    v.iov_base = const_cast<void*>(base);
    v.iov_len = length;
    return v;
}

}  // namespace

Segment::Segment(io::IoBackend& backend, int fd, std::uint32_t id, std::string path, bool locked,
                 bool direct)
    : backend_(backend),
      fd_(fd),
      id_(id),
      path_(std::move(path)),
      locked_(locked),
      direct_(direct) {
    cached_size_ = backend_.size(fd_);
}

Segment::~Segment() {
    if (meta_fd_ >= 0) {
        backend_.close(meta_fd_);
    }
    if (fd_ >= 0) {
        if (locked_) {
            backend_.unlock(fd_);
        }
        backend_.close(fd_);
    }
}

void Segment::unlock() noexcept {
    if (locked_) {
        backend_.unlock(fd_);
        locked_ = false;
    }
}

std::uint64_t Segment::refreshSize() {
    cached_size_ = backend_.size(fd_);
    return cached_size_;
}

void Segment::initialize(std::uint64_t created_ns, std::uint64_t max_record_bytes) {
    max_record_bytes_ = max_record_bytes;
    SegmentHeader header{};
    header.magic = format::kSegmentMagic;
    header.version = format::kSegmentVersion;
    header.segment_id = id_;
    header.created_ns = created_ns;
    header.max_record_bytes = static_cast<std::uint32_t>(max_record_bytes);
    header.header_crc = format::computeHeaderCrc(header);

    // The whole page goes down, not just the 64 bytes of struct: the bytes
    // after it must read as zero, because the frontier is read from there.
    std::string page(kHeaderSize, '\0');
    std::memcpy(page.data(), &header, sizeof(header));
    iovec iov = makeIov(page.data(), page.size());
    backend_.writeAll(fd_, &iov, 1, 0);
    // The header rides an O_DSYNC descriptor when the archive is durable, but
    // a benchmark run may have turned that off; sync once here regardless so
    // that a segment file is never left without a readable header.
    backend_.fsync(fd_);
    write_offset_ = kHeaderSize;
    cached_size_ = kHeaderSize;
}

bool Segment::verifyHeader() {
    SegmentHeader header{};
    if (readAt(0, &header, sizeof(header)) != sizeof(header)) {
        return false;
    }
    if (!format::headerIsValid(header)) {
        return false;
    }
    if (header.segment_id != id_) {
        RAF_LOG_WARN("segment {} carries id {} in its header", path_, header.segment_id);
        return false;
    }
    max_record_bytes_ = format::recordLimit(header);
    return true;
}

std::size_t Segment::readAt(std::uint64_t offset, void* buffer, std::size_t length) {
    if (direct_) {
        return readAligned(offset, buffer, length);
    }
    iovec iov = makeIov(buffer, length);
    return backend_.readSome(fd_, &iov, 1, offset);
}

// The window a random lookup reads through: wide enough for a header and a
// speculated payload, so one refill serves both reads.
ReadWindow& Segment::recordWindow() {
    // No standing readahead either. The buffer is sized for the largest
    // speculation so growth is rare, but each lookup asks for exactly the
    // header and the payload it expects -- filling the buffer every time
    // would read 64 KiB to return a 4 KiB record.
    static thread_local ReadWindow window(0);
    return window;
}

std::size_t Segment::readAligned(std::uint64_t offset, void* buffer, std::size_t length) {
    // One buffer per thread rather than one per segment: random reads share
    // a segment across threads, and this is the path they take.
    // No readahead: these callers have no locality to exploit.
    static thread_local ReadWindow scratch(0);
    // Deliberately not reused between calls. A window pays off where reads
    // are sequential, and the callers that land here -- random lookups, the
    // header, the frontier -- are not.
    scratch.invalidate();
    return scratch.read(backend_, fd_, offset, buffer, length);
}

int Segment::metaFd() {
    if (meta_fd_ < 0) {
        io::OpenFlags flags;  // deliberately not sync: see publishFrontier
        meta_fd_ = backend_.open(path_, flags);
    }
    return meta_fd_;
}

void Segment::disableFrontier(const char* why) {
    frontier_disabled_ = true;
    RAF_LOG_WARN("no longer publishing the frontier of {}: {}", path_, why);
}

// Measurement switch, not a feature. RAF_PUBLISH_FRONTIER=0 takes the second
// write out of the append path so a benchmark can say what it costs; a reader
// then falls back to calling damaged records end-of-data, which is a worse
// diagnostic rather than a wrong answer. Read once, so no append pays for it.
bool frontierEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("RAF_PUBLISH_FRONTIER");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void Segment::publishFrontier(std::uint64_t offset) {
    if (!frontierEnabled() || frontier_disabled_) {
        return;
    }
    try {
        const std::uint64_t value = offset;
        iovec iov = makeIov(&value, sizeof(value));
        backend_.writeAll(metaFd(), &iov, 1, format::kFrontierOffset);
        published_frontier_ = offset;
    } catch (const std::exception& e) {
        // The frontier is an optimisation for readers. Losing it costs them
        // the ability to call a damaged record damaged -- they will call it
        // end of data instead -- which is a worse diagnostic, not a wrong
        // answer. Never fail an append over it.
        //
        // Giving up for good rather than trying again next time. The
        // descriptor is a second one on a file this segment already has
        // open, so the usual reason it cannot be had is the process running
        // out of them -- 512 writers want 1024 descriptors against a default
        // limit of 1024. Retrying then puts a failing open() on every
        // append, which is how a diagnostic that is allowed to be missing
        // turned into a system call per record.
        disableFrontier(e.what());
    }
}

std::uint64_t Segment::publishedFrontier() {
    std::uint64_t value = 0;
    if (readAt(format::kFrontierOffset, &value, sizeof(value)) != sizeof(value)) {
        return 0;
    }
    // Range-check rather than trust: this field carries no checksum, and an
    // overstated frontier would make a reader call unwritten bytes damage.
    if (value < kHeaderSize || value > refreshSize()) {
        return 0;
    }
    return value;
}

void Segment::extendPreallocation() {
    if (preallocate_bytes_ == 0) {
        return;
    }
    // Refill only once the runway is half gone, then refill all of it.
    // Topping up to a full runway after every append would mean writing a
    // record's worth of zeros past the preallocated end each time -- an
    // extending write per append, which is the exact cost this exists to
    // avoid. It made preallocation measure 8x worse than leaving it off.
    if (preallocated_to_ >= write_offset_ + preallocate_bytes_ / 2) {
        return;
    }
    const std::uint64_t want = write_offset_ + preallocate_bytes_;
    // Written, not fallocate'd. An unwritten extent still costs a metadata
    // commit when it is first written to, which is the cost this exists to
    // avoid -- measured at 708 us against 620 us for a plain extend, so
    // fallocate buys nothing here.
    static constexpr std::size_t kChunk = 256u << 10;
    static const std::vector<char> zeros(kChunk, 0);
    std::uint64_t at = std::max(preallocated_to_, write_offset_);
    try {
        // Buffered, through the descriptor without O_DSYNC, then flushed once
        // at the end. Pushing the zeros down the O_DSYNC descriptor would
        // make every 256 KiB chunk its own durable write -- 128 flushes to
        // lay down a 32 MiB runway, where one will do. They only have to be
        // durable before a record lands on those blocks, not as they go.
        const int fd = metaFd();
        while (at < want) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, want - at));
            iovec iov = makeIov(zeros.data(), n);
            backend_.writeAll(fd, &iov, 1, at);
            at += n;
        }
        // Without this the blocks are not allocated yet -- ext4 defers that to
        // writeback -- and the first record to land on them would pay the
        // allocation this exists to avoid.
        backend_.fsync(fd);
        preallocated_to_ = at;
        cached_size_ = std::max(cached_size_, at);
    } catch (const std::exception& e) {
        RAF_LOG_WARN("could not preallocate {} to {}: {}", path_, want, e.what());
        preallocated_to_ = at;
    }
}

void Segment::configurePreallocation(std::uint64_t bytes) {
    preallocate_bytes_ = bytes;
    preallocated_to_ = std::max(write_offset_, cached_size_);
    extendPreallocation();
}

ScanResult Segment::recover() {
    ScanResult result;
    refreshSize();
    // Before the scan, so that frontier() means something while it runs:
    // write_offset_ is still at the header and cannot stand in for it yet.
    published_frontier_ = publishedFrontier();

    std::uint64_t offset = kHeaderSize;
    std::string payload;
    RecordMeta meta{};
    while (true) {
        const RecordStatus status = fetchMeta(offset, meta);
        if (status == RecordStatus::kDamaged) {
            RAF_LOG_ERROR("segment {} has a damaged record header at offset {}", path_, offset);
            break;
        }
        if (status == RecordStatus::kMissing) {
            break;
        }

        detail::resizeUninitialized(payload, meta.length);
        if (readAt(offset + kMetaSize, payload.data(), meta.length) != meta.length) {
            break;  // torn tail: the payload never made it
        }

        result.next_sequence = std::max(result.next_sequence, meta.sequence + 1);
        result.last_timestamp_ns = std::max(result.last_timestamp_ns, meta.timestamp_ns);

        if (crc32c(payload.data(), payload.size()) == meta.data_crc) {
            ++result.record_count;
        } else {
            // A header we trust in front of a payload we do not. Below the
            // frontier the writer acknowledged this record, so it is damage to
            // step over; at or above it the write was cut short and this is
            // the tail.
            if (offset >= frontier()) {
                break;
            }
            RAF_LOG_ERROR("segment {} record at offset {} failed its payload checksum", path_,
                          offset);
            ++result.damaged_records;
        }
        offset += format::recordSpan(meta.length);
    }

    result.end_offset = offset;
    // With preallocation on, everything past the append point is the zero
    // tail this segment wrote on purpose. Cutting it off would only make
    // configurePreallocation write it again, and it would be reported as
    // recovered damage. Leave it: bytes past the frontier are ignored, and
    // the next append overwrites them.
    if (preallocate_bytes_ == 0 && cached_size_ > offset) {
        result.truncated_bytes = cached_size_ - offset;
        backend_.truncate(fd_, offset);
        backend_.fsync(fd_);
        cached_size_ = offset;
    }

    write_offset_ = offset;
    next_sequence_ = result.next_sequence;
    last_timestamp_ns_ = result.last_timestamp_ns;
    return result;
}

void Segment::appendRecords(const std::string_view* values, std::size_t count,
                            INDEX_TYPE* out_index, AppendScratch& scratch) {
    if (count == 0) {
        return;
    }

    // Reserve before taking pointers: the iovecs point into metas, so a
    // reallocation partway through would leave them dangling.
    scratch.metas.clear();
    scratch.metas.reserve(count);
    scratch.iov.clear();
    scratch.iov.reserve(count * 2);

    const auto now =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    // Time never runs backwards inside a segment. Readers merge segments on
    // this field, and a non-decreasing sequence is what makes their order the
    // same as the order the records were appended in.
    const std::uint64_t timestamp = std::max(now, last_timestamp_ns_);

    std::uint64_t offset = write_offset_;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string_view value = values[i];
        RecordMeta meta{};
        meta.magic = format::kRecordMagic;
        meta.length = static_cast<std::uint32_t>(value.size());
        meta.timestamp_ns = timestamp;
        meta.sequence = next_sequence_ + i;
        meta.data_crc = crc32c(value.data(), value.size());
        meta.meta_crc = format::computeMetaCrc(meta);
        scratch.metas.push_back(meta);

        out_index[i] = format::makeIndex(id_, offset);
        offset += format::recordSpan(meta.length);
    }

    for (std::size_t i = 0; i < count; ++i) {
        scratch.iov.push_back(makeIov(&scratch.metas[i], kMetaSize));
        if (!values[i].empty()) {
            scratch.iov.push_back(makeIov(values[i].data(), values[i].size()));
        }
    }

    try {
        backend_.writeAll(fd_, scratch.iov.data(), static_cast<int>(scratch.iov.size()), write_offset_);
    } catch (...) {
        // A write that failed partway leaves a piece of a record on disk. Cut
        // the file back so the next append starts from a clean boundary and a
        // reader never has to reason about the debris.
        try {
            backend_.truncate(fd_, write_offset_);
            backend_.fsync(fd_);
            cached_size_ = write_offset_;
        } catch (const std::exception& nested) {
            RAF_LOG_ERROR("could not roll {} back to offset {}: {}", path_, write_offset_,
                          nested.what());
        }
        throw;
    }

    write_offset_ = offset;
    next_sequence_ += count;
    last_timestamp_ns_ = timestamp;
    cached_size_ = std::max(cached_size_, offset);
    // Only now, with the write durable, may the frontier move: a frontier
    // ahead of what is on disk would have readers call unwritten bytes
    // damage.
    publishFrontier(offset);
    extendPreallocation();
}

RecordStatus Segment::fetchMeta(std::uint64_t offset, RecordMeta& out, ReadWindow* window) {
    // A header read is thirty-two bytes, and on its own it would refill the
    // window with bytes that stop in the middle of a record. Asking for a
    // whole number of records instead, starting here -- which is a record
    // boundary -- means every refill ends on one too, and the records in
    // between cost no system calls at all.
    const std::size_t got =
        (window != nullptr)
            ? window->read(backend_, fd_, offset, &out, kMetaSize, windowFetch())
            : readAt(offset, &out, kMetaSize);
    if (got != kMetaSize) {
        return RecordStatus::kMissing;
    }
    if (format::metaIsValid(out, max_record_bytes_)) {
        return RecordStatus::kOk;
    }
    // Below the frontier the writer has already been past here, so this
    // record was acknowledged and whatever is wrong with it is lost data --
    // including a run of zeros, which means the record is simply gone. At or
    // above the frontier nothing here was ever acknowledged, so the same
    // bytes are an unfinished write or the preallocated tail.
    if (offset < frontier()) {
        return RecordStatus::kDamaged;
    }
    // A reader following a live writer holds a frontier from whenever it last
    // looked. Before calling this the end of the data, re-read it: this is
    // the one place where a stale value changes the verdict.
    published_frontier_ = publishedFrontier();
    return offset < frontier() ? RecordStatus::kDamaged : RecordStatus::kMissing;
}

std::uint64_t Segment::frontier() {
    return std::max(published_frontier_, write_offset_);
}

RecordStatus Segment::readPayload(std::uint64_t offset, const RecordMeta& meta, std::string& out,
                                std::optional<RecordMeta>& next, ReadWindow* window) {
    next.reset();
    RecordMeta peek{};
    detail::resizeUninitialized(out, meta.length);

    std::size_t got = 0;
    if (window != nullptr) {
        // Two copies out of one window, and usually no system call at all:
        // the bytes arrived when an earlier record filled it. The buffered
        // path cannot do this -- its single readv is already one call -- but
        // under O_DIRECT the window is what the alignment rules force, so
        // the readahead comes for free with them.
        //
        // The first read anchors the window on this record's first byte. A
        // refill triggered by the payload read alone would start thirty-two
        // bytes in and end thirty-two bytes into another record, and every
        // refill after it would inherit that: the window would never again
        // hold a whole number of records. It costs nothing when the window
        // already covers the header, which is every record but the one that
        // opens a window.
        RecordMeta anchor{};
        window->read(backend_, fd_, offset, &anchor, kMetaSize, windowFetch(),
                     /*anchor=*/offset);
        got = window->read(backend_, fd_, offset + kMetaSize, out.data(), meta.length,
                           windowFetch(), /*anchor=*/offset);
        if (got == meta.length) {
            // The peek sits on the next record's first byte, so it gets
            // that record's refill rather than one of its own. Left to take
            // the standing readahead, it fetched a window that stopped short
            // of the next payload and was thrown away when the next record
            // asked for one -- two refills for one record, once every
            // sixteen.
            const std::uint64_t next_offset = offset + kMetaSize + meta.length;
            got += window->read(backend_, fd_, next_offset, &peek, kMetaSize, windowFetch(),
                                /*anchor=*/next_offset);
        }
    } else if (direct_) {
        got = readAligned(offset + kMetaSize, out.data(), meta.length);
        if (got == meta.length) {
            got += readAligned(offset + kMetaSize + meta.length, &peek, kMetaSize);
        }
    } else {
        iovec iov[2];
        iov[0] = makeIov(out.data(), meta.length);
        iov[1] = makeIov(&peek, kMetaSize);
        got = backend_.readSome(fd_, iov, 2, offset + kMetaSize);
    }

    if (got < meta.length) {
        return RecordStatus::kMissing;
    }
    const bool peeked = got >= meta.length + kMetaSize && format::metaIsValid(peek, max_record_bytes_);

    if (crc32c(out.data(), out.size()) != meta.data_crc) {
        // The frontier decides this, not whether the prefetch happened to
        // catch a valid header. meta_crc passed, so length is trustworthy and
        // the next record's offset is known either way -- the caller advances
        // by recordSpan and fetches it properly when `next` is empty.
        //
        // The old rule reported a damaged last record as kIncomplete, because
        // nothing follows it to vouch for the segment. That silently demoted
        // an acknowledged record to "never written" and logged nothing.
        if (offset >= frontier()) {
            published_frontier_ = publishedFrontier();
        }
        if (offset >= frontier()) {
            return RecordStatus::kMissing;  // never acknowledged
        }
        if (peeked) {
            next = peek;
        }
        return RecordStatus::kDamaged;  // acknowledged, and now lost
    }

    if (peeked) {
        next = peek;
    }
    last_span_ = format::recordSpan(meta.length);
    return RecordStatus::kOk;
}

// How much a window refill should fetch when it starts at a record boundary:
// as many whole records as a default window holds, so a refill never ends
// part way through one.
//
// Measured, iterating 200 MiB through a direct reader, bytes asked of the
// device per payload byte:
//
//   record     before   after
//   4 KiB      1.0200   1.0200
//   64 KiB     1.0605   1.0602
//   256 KiB    1.3400   1.0369
//   1 MiB      1.0705   1.0105
//
// The 256 KiB case is what this is for: four records to a megabyte, a refill
// that stopped three records in, and a third of the device's bandwidth spent
// re-reading what the last refill had already fetched. 4 KiB was never the
// problem -- 254 records to a window amortise the rounding on their own --
// and it is unchanged.
//
// None of them reaches the 1.008 the arithmetic says is available. Where the
// rest goes is not established.
std::size_t Segment::windowFetch() const {
    const std::size_t span = last_span_;
    if (span == 0) {
        return 0;  // nothing read here yet; the standing readahead will do
    }
    if (span >= ReadWindow::kDefaultCapacity) {
        return span;  // one record is a window on its own
    }
    return ReadWindow::kDefaultCapacity / span * span;
}

std::optional<std::string> Segment::readRecord(std::uint64_t offset) {
    if (offset < kHeaderSize) {
        return std::nullopt;
    }

    // A random read cannot know how long a record is until it has read the
    // header and checked it, which is one system call before the payload read
    // can even start. So guess: records in one segment come from one writer
    // and tend to be the same size, and the guess costs nothing to make.
    //
    // The speculated payload is scattered straight into the string that will
    // be returned, alongside the header going into its own struct, so a
    // correct guess turns two reads into one without copying a byte.
    const std::uint32_t guess = speculated_length_.load(std::memory_order_relaxed);
    RecordMeta meta{};
    std::string value;

    std::size_t got = 0;
    // Under O_DIRECT the same single read happens, through a buffer that
    // satisfies the alignment rules. The window spans the header and a
    // speculated payload, so the payload read below comes out of bytes this
    // one already fetched -- one system call, as in the buffered case.
    ReadWindow* const window = direct_ ? &recordWindow() : nullptr;
    if (window != nullptr) {
        // Each call starts fresh. A record read once while it was still
        // being written must not keep reading short afterwards.
        window->invalidate();
        got = window->read(backend_, fd_, offset, &meta, kMetaSize, kMetaSize + guess);
    } else {
        detail::resizeUninitialized(value, guess);
        iovec iov[2];
        iov[0] = makeIov(&meta, kMetaSize);
        iov[1] = makeIov(value.data(), guess);
        got = backend_.readSome(fd_, iov, guess > 0 ? 2 : 1, offset);
    }

    if (got < kMetaSize || !format::metaIsValid(meta, max_record_bytes_)) {
        return std::nullopt;
    }

    // Speculate on this length next time, unless the records here are big
    // enough that a second read is cheap next to them -- past that, guessing
    // wrong would waste more reading than the system call it saves.
    speculated_length_.store(meta.length <= kMaxSpeculation ? meta.length : 0,
                             std::memory_order_relaxed);

    if (window != nullptr) {
        detail::resizeUninitialized(value, meta.length);
        if (window->read(backend_, fd_, offset + kMetaSize, value.data(), meta.length) !=
            meta.length) {
            return std::nullopt;
        }
    } else if (const std::size_t have = got - kMetaSize; have < meta.length) {
        // The guess was short. Take the payload in one piece rather than
        // growing the buffer around what we already have, which would mean
        // copying it.
        detail::resizeUninitialized(value, meta.length);
        if (readAt(offset + kMetaSize, value.data(), meta.length) != meta.length) {
            return std::nullopt;
        }
    } else {
        value.resize(meta.length);  // the guess ran long; no reallocation
    }

    if (crc32c(value.data(), value.size()) != meta.data_crc) {
        RAF_LOG_WARN("segment {} record at offset {} failed its payload checksum", path_, offset);
        return std::nullopt;
    }
    return value;  // moved into the optional, not copied
}

}  // namespace raf::store
