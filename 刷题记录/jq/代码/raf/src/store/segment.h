// Store layer: one segment file, and everything that knows the byte layout.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <raf/raf.h>

#include "io/io_backend.h"
#include "store/format.h"
#include "store/read_window.h"

namespace raf::store {

// What a segment scan found. A scan runs when a writer adopts a segment: it
// establishes where to append next and drops whatever trailing bytes a crash
// left behind.
struct ScanResult {
    std::uint64_t end_offset = format::kHeaderSize;
    std::uint64_t record_count = 0;
    std::uint64_t damaged_records = 0;  // good header, payload failed its checksum
    std::uint64_t truncated_bytes = 0;
    std::uint64_t next_sequence = 0;
    std::uint64_t last_timestamp_ns = 0;
};

// What came back from reading part of a record -- a header or a payload, the
// same three outcomes either way.
//
// Which of the two failures a bad read means is decided by the published
// frontier, never by what the bytes look like. Below it the writer has
// already been past, so the record was acknowledged and anything wrong with
// it is lost data. At or above it nothing was ever acknowledged -- the
// writer may be mid-write, or may have died mid-write, and raf owes the
// caller nothing either way.
//
// What a caller does about kDamaged does depend on which half it was
// reading, and that belongs to the caller: a damaged header leaves the next
// record's offset unknowable, so iteration over that segment stops, while a
// damaged payload sits behind a header whose checksum passed, so the record
// is skipped and the segment carries on.
enum class RecordStatus {
    kOk,
    // Nothing readable here. The caller may call again later; whether that
    // will ever succeed is the writer's business, not something raf tracks.
    kMissing,
    // Acknowledged, and wrong.
    kDamaged,
};

// Scratch buffers a writer hands to appendRecords so that appending allocates
// nothing per call.
struct AppendScratch {
    std::vector<format::RecordMeta> metas;
    std::vector<iovec> iov;
};

class Segment {
public:
    // `direct` says the descriptor was opened O_DIRECT, so every read has
    // to be rounded out to whole sectors and copied back from an aligned
    // buffer. Writers never set it; see Options::directReads.
    Segment(io::IoBackend& backend, int fd, std::uint32_t id, std::string path, bool locked,
            bool direct = false);
    ~Segment();

    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    std::uint32_t id() const { return id_; }
    const std::string& path() const { return path_; }
    std::uint64_t writeOffset() const { return write_offset_; }
    std::uint64_t nextSequence() const { return next_sequence_; }

    // Size as of the last time we asked the kernel. Iteration works off the
    // cached value and refreshes only when it has run out of records, which
    // keeps fstat off the per-record path.
    std::uint64_t cachedSize() const { return cached_size_; }
    std::uint64_t refreshSize();

    // Writes the segment header into a brand new file and makes it durable.
    // `max_record_bytes` is written into the header so that readers of this
    // segment, in this process or another, agree on what a plausible record
    // length is.
    void initialize(std::uint64_t created_ns, std::uint64_t max_record_bytes);
    // Reads the header back and checks it. False means this file is not a
    // segment we can use.
    bool verifyHeader();

    // Walks the records, leaves write_offset_ at the first byte that is not
    // part of a complete record, and truncates the file there.
    ScanResult recover();

    // Appends `count` values at the current write offset and fills
    // `out_index`. Every payload goes to the kernel straight out of the
    // caller's buffer -- the only bytes raf assembles are the record headers.
    // On return all of them are durable.
    void appendRecords(const std::string_view* values, std::size_t count, INDEX_TYPE* out_index,
                       AppendScratch& scratch);

    // Random read. nullopt means nothing readable lives at `offset`.
    std::optional<std::string> readRecord(std::uint64_t offset);

    // Payload length this segment expects a record to have, which is what a
    // random read speculates on. Exposed for tests.
    std::uint32_t speculatedLength() const {
        return speculated_length_.load(std::memory_order_relaxed);
    }

    // Iteration: read the header at `offset`.
    //
    // `window`, when given, is the cursor's sliding window: the header
    // usually comes out of bytes a previous record already pulled in, and
    // under O_DIRECT it is also what satisfies the alignment rules. Null
    // means read straight from the file, which is what random lookups do.
    RecordStatus fetchMeta(std::uint64_t offset, format::RecordMeta& out,
                          ReadWindow* window = nullptr);

    // Iteration: read the payload described by `meta`, and in the same call
    // pick up the header that follows it. That second iovec is what keeps
    // iteration at one system call per record without copying the payload.
    //
    // `next` holds that header when the read reached it and it parses --
    // engaged or not, rather than a header beside a flag that says whether to
    // look at it. Disengaged is not an error: the caller knows where the next
    // record starts either way, from `recordSpan(meta.length)`.
    RecordStatus readPayload(std::uint64_t offset, const format::RecordMeta& meta, std::string& out,
                           std::optional<format::RecordMeta>& next,
                           ReadWindow* window = nullptr);

    void unlock() noexcept;

    // How far this segment's writer had got, last time it published. A lower
    // bound: stale is safe, overstated is not, so a value outside the file is
    // discarded rather than trusted.
    std::uint64_t publishedFrontier();

    // Keep `bytes` of zeros beyond the append point, so appends overwrite
    // rather than extend. Costs the zeroing; see Options::preallocateBytes.
    // Call before recover(), which then leaves the zero tail alone instead of
    // truncating it for this to write out again.
    void configurePreallocation(std::uint64_t bytes);

    bool direct() const { return direct_; }

    // What this segment's header says the largest payload in it can be.
    std::uint64_t recordLimit() const { return max_record_bytes_; }

private:
    std::size_t readAt(std::uint64_t offset, void* buffer, std::size_t length);
    // readAt for a descriptor opened O_DIRECT: rounds out to whole sectors
    // through a thread-local aligned buffer and copies the slice back.
    std::size_t readAligned(std::uint64_t offset, void* buffer, std::size_t length);
    static ReadWindow& recordWindow();
    std::size_t windowFetch() const;

    // Best knowledge of the append point: what the writer published, or
    // where this segment is appending if it is the writer.
    std::uint64_t frontier();

    // Record the append point in the header. Goes through a descriptor
    // without O_DSYNC: this is a hint, and making it durable would put a
    // second flush on every append to protect a value that is allowed to be
    // stale.
    void publishFrontier(std::uint64_t offset);
    int metaFd();
    // Stop publishing, for good. The frontier is a diagnostic readers can
    // live without; what they cannot live with is a failing open() per
    // append once descriptors run out.
    void disableFrontier(const char* why);

    // Keep `preallocate_bytes_` of zeros written beyond the append point.
    void extendPreallocation();

    io::IoBackend& backend_;
    int fd_;
    std::uint32_t id_;
    std::string path_;
    bool locked_;
    bool direct_;
    std::uint64_t write_offset_ = format::kHeaderSize;
    std::uint64_t next_sequence_ = 0;
    std::uint64_t last_timestamp_ns_ = 0;
    std::uint64_t cached_size_ = 0;
    // From the header on a segment that was opened, from Options on one that
    // was created. Every metaIsValid() in this segment uses it.
    std::uint64_t max_record_bytes_ = kMaxRecordSize;
    std::uint64_t preallocate_bytes_ = 0;
    std::uint64_t preallocated_to_ = 0;
    // Cached so that iteration does not re-read the header page per record.
    // Refreshed when the append point reaches it, which is the only time a
    // stale value could change a verdict.
    std::uint64_t published_frontier_ = 0;
    int meta_fd_ = -1;
    bool frontier_disabled_ = false;
    // Span of the last record iteration read here, which is what the next
    // window refill is sized against. Zero until the first one.
    std::size_t last_span_ = 0;

    // Length of the last record a random read pulled out of this segment, or
    // zero for "do not speculate". A hint only -- nothing depends on it being
    // right, or on two threads agreeing about it.
    std::atomic<std::uint32_t> speculated_length_{0};
};

}  // namespace raf::store
