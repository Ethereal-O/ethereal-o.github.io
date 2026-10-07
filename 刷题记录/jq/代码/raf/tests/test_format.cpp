#include <gtest/gtest.h>

#include <stdexcept>

#include <raf/raf.h>

#include "store/format.h"

namespace raf {
namespace {

using format::RecordMeta;
using format::SegmentHeader;

RecordMeta sealedMeta(std::uint32_t length = 16) {
    RecordMeta meta{};
    meta.magic = format::kRecordMagic;
    meta.length = length;
    meta.timestamp_ns = 1234567890;
    meta.sequence = 7;
    meta.data_crc = 0xdeadbeef;
    meta.meta_crc = format::computeMetaCrc(meta);
    return meta;
}

TEST(Format, HeaderAndMetaAreFixedSize) {
    EXPECT_EQ(sizeof(SegmentHeader), 64u);
    EXPECT_EQ(sizeof(RecordMeta), 32u);
    EXPECT_EQ(format::kMetaSize, 32u);
    // A page, so that the frontier below has one to itself and records start
    // aligned. The struct keeps its 64 bytes; the rest of the page is zero.
    EXPECT_EQ(format::kHeaderSize, 4096u);
    EXPECT_GE(format::kFrontierOffset, sizeof(SegmentHeader));
    EXPECT_EQ(format::kFrontierOffset % sizeof(std::uint64_t), 0u);
    EXPECT_LE(format::kFrontierOffset + sizeof(std::uint64_t), format::kHeaderSize);
}

// The two checksums in a record header answer different questions: one says
// the header can be trusted, the other says the payload can. Mixing them up
// would mean trusting a length before anything had vouched for it.
TEST(Format, MetaChecksumCoversEverythingExceptItself) {
    const RecordMeta meta = sealedMeta();
    EXPECT_TRUE(format::metaIsValid(meta, kMaxRecordSize));

    RecordMeta relabelled = meta;
    relabelled.meta_crc = 0;
    EXPECT_EQ(format::computeMetaCrc(relabelled), format::computeMetaCrc(meta))
        << "the header checksum must not feed on itself";
    EXPECT_FALSE(format::metaIsValid(relabelled, kMaxRecordSize));
}

TEST(Format, MetaChecksumNoticesEveryOtherField) {
    const RecordMeta meta = sealedMeta();
    const std::uint32_t crc = format::computeMetaCrc(meta);

    RecordMeta other = meta;
    other.length = meta.length + 1;
    EXPECT_NE(format::computeMetaCrc(other), crc);

    other = meta;
    other.timestamp_ns += 1;
    EXPECT_NE(format::computeMetaCrc(other), crc);

    other = meta;
    other.sequence += 1;
    EXPECT_NE(format::computeMetaCrc(other), crc);

    // The payload checksum lives in the header, so the header checksum has to
    // protect it too -- otherwise damage could redirect the payload check.
    other = meta;
    other.data_crc ^= 1u;
    EXPECT_NE(format::computeMetaCrc(other), crc);
}

TEST(Format, MetaValidationRejectsGarbage) {
    RecordMeta meta = sealedMeta();
    meta.magic ^= 1u;
    EXPECT_FALSE(format::metaIsValid(meta, kMaxRecordSize));

    // A length raf would never have written, even with a checksum over it.
    meta = RecordMeta{};
    meta.magic = format::kRecordMagic;
    meta.length = static_cast<std::uint32_t>(kMaxRecordSize) + 1;
    meta.meta_crc = format::computeMetaCrc(meta);
    EXPECT_FALSE(format::metaIsValid(meta, kMaxRecordSize));

    EXPECT_FALSE(format::metaIsValid(RecordMeta{}, kMaxRecordSize));
}

TEST(Format, SegmentHeaderChecksumCoversEverythingExceptItself) {
    SegmentHeader header{};
    header.magic = format::kSegmentMagic;
    header.version = format::kSegmentVersion;
    header.segment_id = 3;
    header.created_ns = 99;
    header.header_crc = format::computeHeaderCrc(header);
    EXPECT_TRUE(format::headerIsValid(header));

    SegmentHeader other = header;
    other.header_crc = 0;
    EXPECT_EQ(format::computeHeaderCrc(other), format::computeHeaderCrc(header));

    other = header;
    other.version += 1;
    EXPECT_FALSE(format::headerIsValid(other));

    other = header;
    other.magic ^= 1u;
    EXPECT_FALSE(format::headerIsValid(other));
}

TEST(Format, IndexPacksSegmentAndOffset) {
    struct Case {
        std::uint32_t segment;
        std::uint64_t offset;
    };
    for (const Case& c : {Case{0, 0}, Case{0, format::kHeaderSize}, Case{1, 64},
                          Case{kMaxSegments - 1, (1ull << 48) - 1}, Case{12345, 1ull << 40}}) {
        const INDEX_TYPE index = format::makeIndex(c.segment, c.offset);
        EXPECT_GE(index, 0) << "indexes stay non-negative so that a negative one is a clear error";
        EXPECT_EQ(segmentOf(index), c.segment);
        EXPECT_EQ(offsetOf(index), c.offset);
    }
}

TEST(Format, NegativeIndexesAreRejected) {
    EXPECT_THROW(segmentOf(-1), std::logic_error);
    EXPECT_THROW(offsetOf(kInvalidIndex), std::logic_error);
}

TEST(Format, RecordSpanIncludesTheHeader) {
    EXPECT_EQ(format::recordSpan(0), format::kMetaSize);
    EXPECT_EQ(format::recordSpan(100), format::kMetaSize + 100);
    // No overflow: the span of the largest record still fits comfortably.
    EXPECT_EQ(format::recordSpan(static_cast<std::uint32_t>(kMaxRecordSize)),
              format::kMetaSize + kMaxRecordSize);
}

}  // namespace
}  // namespace raf
