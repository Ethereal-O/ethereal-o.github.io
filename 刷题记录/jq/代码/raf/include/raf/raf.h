// RAF -- Record Archive File.
//
// An archive stores opaque binary records. Writers only append; readers can
// walk the archive in order or jump straight to a record by index. `append()`
// returning means the record is on stable storage: a crash right after the
// call still leaves the record readable.
//
// The layering behind this header follows docs/stage3.md: an api layer that
// owns the user-visible semantics (this file), a store layer that owns the file
// format, and a backend that owns the system calls.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raf {

// A record index. The design doc settles on a plain integer rather than a
// struct, but one integer has to address a record inside one of several
// segment files, so the bits are split: 15 bits of segment id in 62..48 and 48
// bits of byte offset in 47..0. Bit 63 stays clear, which keeps every valid
// index non-negative and makes a negative value unambiguously bogus.
using INDEX_TYPE = std::int64_t;

inline constexpr INDEX_TYPE kInvalidIndex = -1;

// Largest record raf accepts. Bounding the length is what lets a reader tell a
// half-written tail record from real corruption: past this much slack, a
// broken header cannot be explained by a write still in flight.
// Default limit on one record's payload; Options::maxRecordBytes moves it.
inline constexpr std::uint64_t kMaxRecordSize = 64ull << 20;

// And the limit on that limit, which the format and the read path impose
// rather than choose. A record's length is a 32-bit field, and a payload is
// read in one call whose transfer the kernel caps just under 2 GiB -- past
// that a read comes back short, which raf reads as "not written yet". One
// gibibyte is the round number safely inside both.
//
// There is therefore no "unlimited": a bigger record than this needs a wider
// length field, which is a format change.
inline constexpr std::uint64_t kRecordSizeCeiling = 1ull << 30;

// Limits implied by the index encoding.
inline constexpr std::uint32_t kMaxSegments = 1u << 15;
inline constexpr std::uint64_t kMaxSegmentBytes = 1ull << 48;

struct Options {
    // Write this many zero bytes ahead of the append point, so that appending
    // overwrites blocks that already exist instead of extending the file.
    // Extending costs a filesystem metadata commit on every append, and
    // skipping it is worth 1.8x: measured on a consumer NVMe at 335 us per
    // 4 KiB durable append against 613 us without.
    //
    // That 1.8x holds up. The same drive falls off a cliff under sustained
    // small durable writes whether this is on or not -- 4427 us per append
    // afterwards with it off, 2128 us with it on -- so the cliff belongs to
    // the device, not to this option, and the ratio survives it: 1.84x
    // before, 1.89x after.
    //
    // It is still off by default, because it writes twice the bytes. That
    // buys nothing on a device whose flush is cheap, costs flash endurance,
    // and brings the cliff forward in records written -- 82 MB of records
    // against 132 MB -- even though it is the later of the two in device
    // bytes, since the zeros go down as large sequential writes.
    //
    // Measure before turning it on; bench/physical/sustained.sh --curves EF
    // runs exactly this comparison. appendBatch beats both and costs nothing.
    std::uint64_t preallocateBytes = 0;
    // A writer rolls over to a fresh segment before a write would push its
    // current one past this. Keeping segments bounded keeps recovery scans and
    // the reader's per-file state bounded too.
    std::uint64_t maxSegmentBytes = 1ull << 40;

    // Largest payload append() will take. Raising it costs memory rather
    // than disk -- a record is assembled whole in one std::string on the way
    // in and on the way out -- and it is recorded in each segment's header,
    // so a reader that was never told about it still knows what lengths this
    // segment's writer could have produced.
    //
    // Clamped to kRecordSizeCeiling. Zero means kMaxRecordSize.
    std::uint64_t maxRecordBytes = kMaxRecordSize;

    // Read segments with O_DIRECT: the drive transfers straight into the
    // reader's buffer and the page cache is not involved. On by default,
    // because a 4 KiB record is 4128 bytes with its header and so straddles
    // two pages, which through the cache costs it twice its own size. On the
    // drive this was measured against -- a 40 GiB archive on a 31 GiB
    // machine, cold -- that is 713 MiB/s against 462 on random reads and
    // 1805 against 940 on sequential ones.
    //
    // Turn it off when the working set fits in memory. A cache hit cannot be
    // beaten by making the miss cheaper: over a 394 MiB archive served
    // entirely from cache, buffered reads measured 2246 MiB/s against 35,
    // with p50 at 1.4 us against 104. Also turn it off on a filesystem that
    // cannot do O_DIRECT at all, tmpfs being the one most likely to matter
    // -- opening a segment there fails outright.
    //
    // Only readers honour it. Writers stay buffered with O_DSYNC.
    bool directReads = true;

    // Open segments with O_DSYNC. Turning this off makes `append()` return
    // before the record is durable and so breaks the contract this API is
    // built around; it exists for benchmarking the layers above the disk.
    bool sync = true;
};

// Appends records to an archive.
//
// One writer owns one segment file exclusively (flock), so several writers --
// threads or processes -- can append to the same archive at once without
// coordinating. A single writer serializes its own appends, so the way to get
// more write parallelism is more writers, one per thread.
//
// All methods are safe to call concurrently on the same object.
class RecordWriter {
public:
    // Opens the archive at `filepath`, creating it if it is not there, and
    // takes ownership of one segment inside it.
    explicit RecordWriter(const std::string& filepath);
    RecordWriter(const std::string& filepath, const Options& options);

    RecordWriter(RecordWriter&&) noexcept;
    RecordWriter& operator=(RecordWriter&&) noexcept;
    ~RecordWriter();

    // Appends one record and returns its index. On return the record is
    // durable. Throws std::logic_error if the writer is closed or the record
    // is longer than Options::maxRecordBytes, std::system_error if the write
    // fails.
    INDEX_TYPE append(const std::string& value);
    INDEX_TYPE append(std::string_view value);

    // Appends every value in order and returns their indexes. Records go to
    // disk in as few writes as the iovec limit allows. On return all of them
    // are durable; if the call throws or the process dies partway, an
    // unspecified prefix may have made it, which is the same freedom a crash
    // during `append()` has.
    std::vector<INDEX_TYPE> appendBatch(const std::vector<std::string>& values);
    std::vector<INDEX_TYPE> appendBatch(const std::vector<std::string_view>& values);

    // Releases the segment. Appending afterwards throws std::logic_error.
    // Calling close() twice is not an error; the destructor closes too.
    void close();

    // Segment this writer owns. Useful for tests and for reasoning about which
    // records share a total order.
    std::uint32_t segmentId() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reads records from an archive.
//
// `read()` is a random lookup by index. `readNext()` walks the archive.
//
// The iteration contract, with several writers appending to several segments:
//
//   * Records that were already complete when the reader opened come out in
//     one order, and every reader that opens on the same set of segment tails
//     sees that same order.
//   * Records appended after the reader opened keep the order of the writer
//     that wrote them, but no order is defined between writers.
//
// All methods are safe to call concurrently on the same object.
class RecordReader {
public:
    explicit RecordReader(const std::string& filepath);
    // Only Options::directReads is read; the rest describe writing.
    RecordReader(const std::string& filepath, const Options& options);

    RecordReader(RecordReader&&) noexcept;
    RecordReader& operator=(RecordReader&&) noexcept;
    ~RecordReader();

    // Returns the record at `index`, or nullopt if nothing readable lives
    // there -- either nothing was written or what is there fails its checksum.
    // Throws std::logic_error if the reader is closed or the index is
    // malformed, std::system_error if the read fails.
    std::optional<std::string> read(const INDEX_TYPE& index);

    // Returns the next record in iteration order, or nullopt once the reader
    // has caught up with every segment. A later call can return more: records
    // appended in the meantime, and segments created in the meantime, both
    // become visible.
    std::optional<std::string> readNext();

    // Index of the record the last readNext() returned; kInvalidIndex before
    // the first one.
    INDEX_TYPE currentIndex() const;

    // Restarts iteration from the beginning of the archive.
    void rewind();

    // Closes the archive. Reading afterwards throws std::logic_error.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Splits an index into the segment it lives in and the offset inside it.
std::uint32_t segmentOf(INDEX_TYPE index);
std::uint64_t offsetOf(INDEX_TYPE index);

}  // namespace raf
