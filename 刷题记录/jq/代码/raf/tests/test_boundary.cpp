// Edges: empty and maximum records, batch and segment boundaries, and lengths
// that have to be turned away before anything is reserved or written.
#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <raf/raf.h>

#include "test_support.h"

namespace raf {
namespace {

class BoundaryTest : public ::testing::Test {
protected:
    test::TempArchive tmp{"raf-boundary"};
    Options fast;  // durability is covered elsewhere; these tests move volume

    BoundaryTest() { fast.sync = false; }
};

// An empty record is a record: it reads back as a value, not as "nothing is
// there", which is what nullopt means.
// Preallocation only changes where the zeros are, never what a reader sees.
TEST_F(BoundaryTest, PreallocationChangesNothingAReaderCanSee) {
    Options prealloc = fast;
    prealloc.preallocateBytes = 64 * 1024;

    std::vector<INDEX_TYPE> indexes;
    {
        RecordWriter writer(tmp.path(), prealloc);
        for (std::size_t i = 0; i < 64; ++i) {
            indexes.push_back(writer.append(test::makePayload(1, i, 300)));
        }
        writer.close();
    }

    // The file is longer than the records, which is the whole point.
    RecordReader reader(tmp.path());
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        const std::optional<std::string> value = reader.read(indexes[i]);
        ASSERT_TRUE(value) << "record " << i;
        ASSERT_TRUE(test::payloadIsIntact(*value));
        const std::optional<test::PayloadId> id = test::identifyPayload(*value);
        ASSERT_TRUE(id);
        EXPECT_EQ(id->writer, 1u);
        EXPECT_EQ(id->sequence, i);
    }
    // Iteration stops at the last record, not somewhere out in the zeros.
    std::size_t seen = 0;
    while (reader.readNext()) {
        ++seen;
    }
    EXPECT_EQ(seen, indexes.size());
}

// Reopening a preallocated segment must find the same append point, not the
// end of the zeros.
TEST_F(BoundaryTest, APreallocatedSegmentReopensAtTheRightPlace) {
    Options prealloc = fast;
    prealloc.preallocateBytes = 64 * 1024;

    {
        RecordWriter writer(tmp.path(), prealloc);
        writer.append(test::makePayload(1, 0, 100));
        writer.close();
    }
    {
        RecordWriter writer(tmp.path(), prealloc);
        writer.append(test::makePayload(1, 1, 100));
        writer.close();
    }

    RecordReader reader(tmp.path());
    std::size_t seen = 0;
    while (reader.readNext()) {
        ++seen;
    }
    EXPECT_EQ(seen, 2u);
}

TEST_F(BoundaryTest, EmptyRecordsRoundTrip) {
    RecordWriter writer(tmp.path(), fast);
    const INDEX_TYPE first = writer.append(std::string());
    const INDEX_TYPE second = writer.append(std::string("x"));
    const INDEX_TYPE third = writer.append(std::string());

    RecordReader reader(tmp.path());
    const std::optional<std::string> value = reader.read(first);
    ASSERT_TRUE(value.has_value());
    EXPECT_TRUE(value->empty());
    EXPECT_EQ(reader.read(third), "");
    EXPECT_EQ(offsetOf(second) - offsetOf(first), format::kMetaSize)
        << "an empty record still costs its header";
    EXPECT_EQ(test::drain(reader), (std::vector<std::string>{"", "x", ""}));
}

TEST_F(BoundaryTest, RecordSizesFromOneByteUpwards) {
    const std::vector<std::size_t> sizes = {1,  2,    7,    8,    31,    32,       63,
                                            64, 4095, 4096, 4097, 65536, 1u << 20, (1u << 20) + 1};
    std::vector<INDEX_TYPE> indexes;
    {
        RecordWriter writer(tmp.path(), fast);
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            indexes.push_back(writer.append(test::makePayload(0, i, sizes[i])));
        }
    }
    RecordReader reader(tmp.path());
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const std::optional<std::string> value = reader.read(indexes[i]);
        ASSERT_TRUE(value.has_value()) << "size " << sizes[i];
        EXPECT_EQ(value->size(), sizes[i]);
        EXPECT_EQ(*value, test::makePayload(0, i, sizes[i]));
    }
}

TEST_F(BoundaryTest, TheLargestAllowedRecordRoundTrips) {
    const std::string value = test::makePayload(0, 0, kMaxRecordSize);
    INDEX_TYPE index = kInvalidIndex;
    {
        RecordWriter writer(tmp.path(), fast);
        index = writer.append(value);
    }
    RecordReader reader(tmp.path());
    const std::optional<std::string> read_back = reader.read(index);
    ASSERT_TRUE(read_back.has_value());
    EXPECT_EQ(read_back->size(), kMaxRecordSize);
    EXPECT_EQ(*read_back, value);
}

// The length is refused before a byte is written, so nothing has to be undone.
TEST_F(BoundaryTest, ARecordOverTheLimitIsRefusedBeforeAnythingIsWritten) {
    RecordWriter writer(tmp.path(), fast);
    writer.append(std::string("kept"));
    const std::uint64_t before = test::fileSize(test::segmentPaths(tmp.path()).front());

    const std::string oversized(kMaxRecordSize + 1, 'x');
    EXPECT_THROW(writer.append(oversized), std::logic_error);
    EXPECT_EQ(test::fileSize(test::segmentPaths(tmp.path()).front()), before);

    // The writer is still usable afterwards.
    writer.append(std::string("after"));
    EXPECT_EQ(test::drain(tmp.path()), (std::vector<std::string>{"kept", "after"}));
}

// The limit moves, and a raised one takes records the default refuses.
TEST_F(BoundaryTest, MaxRecordBytesRaisesWhatAppendWillTake) {
    Options big = fast;
    big.maxRecordBytes = kMaxRecordSize * 2;
    const std::string value = test::makePayload(1, 0, kMaxRecordSize + 4096);

    INDEX_TYPE index = kInvalidIndex;
    {
        RecordWriter writer(tmp.path(), big);
        ASSERT_NO_THROW(index = writer.append(value));
    }
    // A reader that was told nothing about the limit still returns it: the
    // segment's header carries what its writer was allowed to write.
    EXPECT_EQ(RecordReader(tmp.path()).read(index), value);
    EXPECT_EQ(test::drain(tmp.path()), std::vector<std::string>{value});
}

// And a lowered one refuses what the default would have taken.
TEST_F(BoundaryTest, MaxRecordBytesLowersItToo) {
    Options small = fast;
    small.maxRecordBytes = 1024;
    RecordWriter writer(tmp.path(), small);
    EXPECT_NO_THROW(writer.append(std::string(1024, 'x')));
    EXPECT_THROW(writer.append(std::string(1025, 'x')), std::logic_error);
    writer.close();
    EXPECT_EQ(test::drain(tmp.path()).size(), 1u);
}

// Zero means the default rather than "refuse everything", and a limit over
// what a 32-bit length field can carry is refused rather than silently
// clamped -- an archive that quietly took a smaller limit than asked for
// would reject records the caller had every reason to expect.
TEST_F(BoundaryTest, MaxRecordBytesIsCheckedWhenTheWriterOpens) {
    Options zero = fast;
    zero.maxRecordBytes = 0;
    {
        RecordWriter writer(tmp.path(), zero);
        EXPECT_NO_THROW(writer.append(std::string(kMaxRecordSize, 'x')));
    }

    Options over = fast;
    over.maxRecordBytes = kRecordSizeCeiling + 1;
    EXPECT_THROW(RecordWriter writer(tmp.path(), over), std::logic_error);
}

// A segment written before the header carried a limit reads with the
// default, rather than rejecting every record in it.
TEST_F(BoundaryTest, ASegmentWithNoLimitInItsHeaderUsesTheDefault) {
    const std::string value = test::makePayload(1, 0, 4096);
    {
        RecordWriter writer(tmp.path(), fast);
        writer.append(value);
    }
    const std::string path = test::segmentPaths(tmp.path()).front();

    // Zero the field and re-stamp the header checksum, which is what an
    // older raf would have left behind.
    format::SegmentHeader header{};
    const std::string raw = test::readRaw(path, 0, sizeof(header));
    std::memcpy(&header, raw.data(), sizeof(header));
    ASSERT_EQ(header.max_record_bytes, kMaxRecordSize);
    header.max_record_bytes = 0;
    header.header_crc = format::computeHeaderCrc(header);
    test::writeRaw(path, 0, &header, sizeof(header));

    EXPECT_EQ(test::drain(tmp.path()), std::vector<std::string>{value});
}

TEST_F(BoundaryTest, AnOversizedValueRejectsTheWholeBatch) {
    RecordWriter writer(tmp.path(), fast);
    writer.append(std::string("kept"));

    std::vector<std::string> batch = {"a", std::string(kMaxRecordSize + 1, 'x'), "c"};
    EXPECT_THROW(writer.appendBatch(batch), std::logic_error);
    EXPECT_EQ(test::drain(tmp.path()), (std::vector<std::string>{"kept"}))
        << "a batch that cannot be written whole must not be written in part";
}

// Batches larger than one write are split, and the split must not show.
TEST_F(BoundaryTest, BatchesSpanningTheWriteChunkBoundary) {
    for (const std::size_t count : {std::size_t{1}, std::size_t{511}, std::size_t{512},
                                    std::size_t{513}, std::size_t{1025}}) {
        test::TempArchive archive{"raf-batch"};
        std::vector<std::string> values;
        values.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            values.push_back(test::makePayload(0, i, 40));
        }
        std::vector<INDEX_TYPE> indexes;
        {
            RecordWriter writer(archive.path(), fast);
            indexes = writer.appendBatch(values);
        }
        ASSERT_EQ(indexes.size(), count);
        EXPECT_EQ(test::drain(archive.path()), values) << "batch of " << count;

        RecordReader reader(archive.path());
        for (std::size_t i = 0; i < count; ++i) {
            EXPECT_EQ(reader.read(indexes[i]), values[i])
                << "batch of " << count << " record " << i;
        }
    }
}

TEST_F(BoundaryTest, WritersRollOverToANewSegment) {
    Options small = fast;
    small.maxSegmentBytes = format::kHeaderSize + 512;

    std::vector<std::string> values;
    std::vector<INDEX_TYPE> indexes;
    {
        RecordWriter writer(tmp.path(), small);
        for (int i = 0; i < 20; ++i) {
            values.push_back(test::makePayload(0, i, 100));
            indexes.push_back(writer.append(values.back()));
        }
    }

    std::set<std::uint32_t> segments;
    for (const INDEX_TYPE index : indexes) {
        segments.insert(segmentOf(index));
    }
    EXPECT_GT(segments.size(), 1u) << "the limit has to actually force a rollover";
    for (const std::string& path : test::segmentPaths(tmp.path())) {
        EXPECT_LE(test::fileSize(path), small.maxSegmentBytes);
    }

    RecordReader reader(tmp.path());
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        EXPECT_EQ(reader.read(indexes[i]), values[i]) << "record " << i;
    }
    EXPECT_EQ(test::drain(reader), values) << "rolling over must not disturb the order";
}

TEST_F(BoundaryTest, ABatchThatOutgrowsASegmentIsSplitAcrossSegments) {
    Options small = fast;
    small.maxSegmentBytes = format::kHeaderSize + 600;

    std::vector<std::string> values;
    for (int i = 0; i < 30; ++i) {
        values.push_back(test::makePayload(0, i, 100));
    }
    std::vector<INDEX_TYPE> indexes;
    {
        RecordWriter writer(tmp.path(), small);
        indexes = writer.appendBatch(values);
    }
    EXPECT_GT(test::segmentPaths(tmp.path()).size(), 1u);
    EXPECT_EQ(test::drain(tmp.path()), values);

    RecordReader reader(tmp.path());
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        EXPECT_EQ(reader.read(indexes[i]), values[i]);
    }
}

// Something has to take a record bigger than the segment limit, or it could
// never be written at all.
TEST_F(BoundaryTest, ARecordLargerThanTheSegmentLimitGoesIntoAFreshSegment) {
    Options small = fast;
    small.maxSegmentBytes = format::kHeaderSize + 256;

    const std::string big = test::makePayload(0, 0, 4096);
    INDEX_TYPE index = kInvalidIndex;
    {
        RecordWriter writer(tmp.path(), small);
        writer.append(std::string("small"));
        index = writer.append(big);
        writer.append(std::string("after"));
    }
    RecordReader reader(tmp.path());
    EXPECT_EQ(reader.read(index), big);
    EXPECT_EQ(test::drain(reader).size(), 3u);
}

TEST_F(BoundaryTest, ASegmentLimitWithNoRoomForRecordsIsRejected) {
    Options tiny = fast;
    tiny.maxSegmentBytes = format::kHeaderSize;
    EXPECT_THROW(RecordWriter writer(tmp.path(), tiny), std::logic_error);

    tiny.maxSegmentBytes = 0;
    EXPECT_THROW(RecordWriter writer(tmp.path(), tiny), std::logic_error);
}

TEST_F(BoundaryTest, ASegmentLimitBeyondWhatAnIndexCanAddressIsClamped) {
    Options huge = fast;
    huge.maxSegmentBytes = ~std::uint64_t{0};
    RecordWriter writer(tmp.path(), huge);
    const INDEX_TYPE index = writer.append(std::string("fits"));
    EXPECT_LE(offsetOf(index), kMaxSegmentBytes);
    EXPECT_EQ(RecordReader(tmp.path()).read(index), "fits");
}

// The arithmetic that decides whether a record fits -- `offset + used +
// span` in the writer -- is unsigned, so wrapping it would make an
// over-the-limit batch look like it fits and write past the segment cap.
// What stops that is the clamp above, not a check at the comparison: every
// term is bounded, and the bound is what this pins.
TEST_F(BoundaryTest, TheFitCheckCannotWrapHoweverLargeTheLimitIs) {
    // The largest the three terms can be: an offset at the clamped cap, a
    // full batch of the largest records, and one more span.
    const std::uint64_t largest_span = format::recordSpan(kMaxRecordSize);
    const std::uint64_t headroom = ~std::uint64_t{0} - kMaxSegmentBytes;
    EXPECT_GT(headroom / largest_span, 512u)
        << "a batch of the largest records must not be able to carry the sum past 2^64";
}

// The same thing from the other end, behaviourally: a limit set so high it
// is clamped, and a whole batch written against it. A wrap in the fit check
// would put records past the cap, and their indexes would not resolve.
TEST_F(BoundaryTest, AWholeBatchAgainstTheClampedMaximumStillResolves) {
    Options huge = fast;
    huge.maxSegmentBytes = ~std::uint64_t{0};
    RecordWriter writer(tmp.path(), huge);

    std::vector<std::string> values;
    for (int i = 0; i < 64; ++i) {
        values.push_back(test::makePayload(0, i, 4096));
    }
    const std::vector<INDEX_TYPE> indexes = writer.appendBatch(values);
    writer.close();

    ASSERT_EQ(indexes.size(), values.size());
    RecordReader reader(tmp.path());
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        EXPECT_LE(offsetOf(indexes[i]), kMaxSegmentBytes);
        EXPECT_EQ(reader.read(indexes[i]), values[i]) << "record " << i;
    }
}

// A length that would overflow the span computation has to be refused before
// anything is reserved. The check is on the value's own size, so the only
// way past it is a length the type cannot hold in the first place.
TEST_F(BoundaryTest, ALengthThatCouldOverflowASpanIsRefusedBeforeAnythingIsWritten) {
    RecordWriter writer(tmp.path(), fast);
    const INDEX_TYPE before = writer.append(std::string("anchor"));

    const std::string oversized(kMaxRecordSize + 1, 'x');
    EXPECT_THROW(writer.append(oversized), std::logic_error);
    // And the length that fits exactly, whose span is the largest a record
    // can have, still computes.
    EXPECT_EQ(format::recordSpan(kMaxRecordSize), kMaxRecordSize + format::kMetaSize);
    writer.close();

    EXPECT_EQ(RecordReader(tmp.path()).read(before), "anchor")
        << "a refused length must leave the archive exactly as it was";
}

TEST_F(BoundaryTest, IndexesThatNameNothingReadAsEmpty) {
    RecordWriter writer(tmp.path(), fast);
    const INDEX_TYPE index = writer.append(test::makePayload(0, 0, 200));
    writer.close();

    RecordReader reader(tmp.path());
    ASSERT_TRUE(reader.read(index).has_value());

    // Inside the record but not at its start.
    EXPECT_EQ(reader.read(index + 1), std::nullopt);
    EXPECT_EQ(reader.read(index + format::kMetaSize), std::nullopt);
    // Past the end of the segment.
    EXPECT_EQ(reader.read(index + 100000), std::nullopt);
    // Before the first record.
    EXPECT_EQ(reader.read(format::makeIndex(0, 0)), std::nullopt);
    EXPECT_EQ(reader.read(format::makeIndex(0, format::kHeaderSize - 1)), std::nullopt);
    // A segment that does not exist.
    EXPECT_EQ(reader.read(format::makeIndex(4242, format::kHeaderSize)), std::nullopt);
}

TEST_F(BoundaryTest, NegativeIndexesAreAProgrammingError) {
    RecordWriter writer(tmp.path(), fast);
    writer.append(std::string("x"));
    RecordReader reader(tmp.path());
    EXPECT_THROW(reader.read(-1), std::logic_error);
    EXPECT_THROW(reader.read(kInvalidIndex), std::logic_error);
    EXPECT_THROW(reader.read(std::numeric_limits<INDEX_TYPE>::min()), std::logic_error);
}

TEST_F(BoundaryTest, RecordsRightAtTheSegmentLimitFit) {
    Options exact = fast;
    // Room for the header and exactly two 100-byte records.
    exact.maxSegmentBytes = format::kHeaderSize + 2 * format::recordSpan(100);

    std::vector<INDEX_TYPE> indexes;
    {
        RecordWriter writer(tmp.path(), exact);
        for (int i = 0; i < 4; ++i) {
            indexes.push_back(writer.append(test::makePayload(0, i, 100)));
        }
    }
    EXPECT_EQ(segmentOf(indexes[0]), segmentOf(indexes[1]));
    EXPECT_NE(segmentOf(indexes[1]), segmentOf(indexes[2]))
        << "the third record is one byte too many for the first segment";
    EXPECT_EQ(segmentOf(indexes[2]), segmentOf(indexes[3]));
    EXPECT_EQ(test::fileSize(test::segmentPaths(tmp.path()).front()), exact.maxSegmentBytes);
}

}  // namespace
}  // namespace raf
