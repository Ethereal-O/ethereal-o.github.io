// On-disk layout of a segment file.
//
//   [ SegmentHeader | RecordMeta data | RecordMeta data | ... ]
//
// The doc calls this the "record" format: metadata and payload sit next to
// each other so one write puts both on disk, which is what lets a single
// O_DSYNC write make a record durable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <raf/raf.h>

#include "common/crc32c.h"

namespace raf::format {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "the on-disk format stores integers little-endian");

inline constexpr std::uint64_t kSegmentMagic = 0x3147455330464152ull;  // "RAF0SEG1"
inline constexpr std::uint32_t kSegmentVersion = 2;
inline constexpr std::uint32_t kRecordMagic = 0x31434552u;  // "REC1"

// The immutable part of a segment's header. Written once and never touched
// again, which is what lets a single crc cover all of it.
struct SegmentHeader {
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t segment_id;
    std::uint64_t created_ns;
    // The largest payload this segment's writer was allowed to append. A
    // reader needs it to tell a header from a run of bytes that happens to
    // look like one, and a reader is usually another process with no access
    // to the Options the writer used -- so it travels with the segment. Zero
    // means a segment written before this field existed; kMaxRecordSize is
    // what those used.
    std::uint32_t max_record_bytes;
    std::uint8_t reserved[32];
    // crc32c over the 60 bytes above. Last so that "everything before this
    // field" needs no offset arithmetic at the call site.
    std::uint32_t header_crc;
};
static_assert(sizeof(SegmentHeader) == 64);
static_assert(offsetof(SegmentHeader, header_crc) == 60);

// Precedes every record's payload.
//
// Two checksums, and they cover different things on purpose. `meta_crc` covers
// this struct except itself, so a reader can trust `length` before it has read
// -- or even sized a buffer for -- the payload. `data_crc` covers the payload,
// which is only checked once the payload has been read. Folding them into one
// checksum would mean having to read a length the reader cannot trust yet.
struct RecordMeta {
    std::uint32_t magic;
    std::uint32_t length;        // payload bytes that follow
    std::uint64_t timestamp_ns;  // when the writer appended; the reader's merge key
    std::uint64_t sequence;      // index of this record within its segment
    std::uint32_t data_crc;      // crc32c over the payload
    std::uint32_t meta_crc;      // crc32c over the 28 bytes above
};
static_assert(sizeof(RecordMeta) == 32);
static_assert(alignof(RecordMeta) == 8);
static_assert(offsetof(RecordMeta, meta_crc) == 28);

// The header occupies a whole page, for three reasons. The mutable frontier
// below gets a page to itself, so publishing it never dirties a page that
// also holds records. Records then start page-aligned, which halves the read
// amplification on a record whose size is a page multiple. And there is room
// for another field without moving anything.
inline constexpr std::uint64_t kHeaderSize = 4096;
inline constexpr std::uint64_t kMetaSize = sizeof(RecordMeta);

// How far the writer had got, last time it said so. Deliberately outside
// SegmentHeader and outside header_crc: it changes while the segment is
// being written, and recomputing the header's checksum on every append would
// mean a torn update could make the whole header unreadable.
//
// It needs no checksum of its own. Eight aligned bytes inside one sector are
// written atomically, so a crash leaves either the old value or the new one,
// and the writer only ever publishes a value whose records are already
// durable. Both outcomes are therefore valid lower bounds, which is all a
// reader asks of it. A reader must still range-check what it reads, since
// bit rot could put any number there and an overstated frontier would call
// unwritten bytes damage.
inline constexpr std::uint64_t kFrontierOffset = 64;
static_assert(kFrontierOffset >= sizeof(SegmentHeader));
static_assert(kFrontierOffset % sizeof(std::uint64_t) == 0);
static_assert(kFrontierOffset + sizeof(std::uint64_t) <= kHeaderSize);

inline std::uint32_t computeMetaCrc(const RecordMeta& meta) {
    return crc32c(&meta, offsetof(RecordMeta, meta_crc));
}

inline std::uint32_t computeHeaderCrc(const SegmentHeader& header) {
    return crc32c(&header, offsetof(SegmentHeader, header_crc));
}

// The limit a segment header carries, with zero meaning it predates the
// field.
inline std::uint64_t recordLimit(const SegmentHeader& header) {
    return header.max_record_bytes != 0 ? header.max_record_bytes : kMaxRecordSize;
}

// True if the header is self-consistent: it is a record header, its checksum
// matches, and the length it claims is one this segment's writer could have
// written.
//
// The length check is not what rejects garbage -- meta_crc does that -- it
// is what bounds the allocation a reader will make on the strength of it.
inline bool metaIsValid(const RecordMeta& meta, std::uint64_t max_record_bytes) {
    return meta.magic == kRecordMagic && meta.length <= max_record_bytes &&
           meta.meta_crc == computeMetaCrc(meta);
}

inline bool headerIsValid(const SegmentHeader& header) {
    return header.magic == kSegmentMagic && header.version == kSegmentVersion &&
           header.header_crc == computeHeaderCrc(header);
}

inline std::uint64_t recordSpan(std::uint32_t length) {
    return kMetaSize + static_cast<std::uint64_t>(length);
}

// Index packing. Bit 63 stays clear so every index is non-negative.
inline constexpr int kSegmentShift = 48;
inline constexpr std::uint64_t kOffsetMask = (1ull << kSegmentShift) - 1;

inline INDEX_TYPE makeIndex(std::uint32_t segment_id, std::uint64_t offset) {
    return static_cast<INDEX_TYPE>((static_cast<std::uint64_t>(segment_id) << kSegmentShift) |
                                   offset);
}

}  // namespace raf::format
