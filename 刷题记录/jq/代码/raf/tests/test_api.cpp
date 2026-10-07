// The user-visible contract: what the calls return, what they promise, and
// what they do when used out of order.
#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <raf/raf.h>

#include "test_support.h"

namespace raf {
namespace {

class ApiTest : public ::testing::Test {
protected:
    test::TempArchive tmp{"raf-api"};
};

TEST_F(ApiTest, AWriterCreatesTheArchive) {
    EXPECT_FALSE(io::posixBackend().directoryExists(tmp.path()));
    RecordWriter writer(tmp.path());
    EXPECT_TRUE(io::posixBackend().directoryExists(tmp.path()));
    EXPECT_EQ(test::segmentPaths(tmp.path()).size(), 1u);
}

TEST_F(ApiTest, AppendReturnsAnIndexThatReadsBack) {
    RecordWriter writer(tmp.path());
    const INDEX_TYPE first = writer.append(std::string("alpha"));
    const INDEX_TYPE second = writer.append(std::string("beta"));
    EXPECT_NE(first, second);
    EXPECT_GE(first, 0);

    RecordReader reader(tmp.path());
    EXPECT_EQ(reader.read(first), "alpha");
    EXPECT_EQ(reader.read(second), "beta");
    // Reading twice gives the same answer; nothing is consumed.
    EXPECT_EQ(reader.read(first), "alpha");
}

TEST_F(ApiTest, AppendBatchReturnsOneIndexPerValue) {
    RecordWriter writer(tmp.path());
    const std::vector<std::string> values = {"a", "bb", "ccc", "", "ddddd"};
    const std::vector<INDEX_TYPE> indexes = writer.appendBatch(values);
    ASSERT_EQ(indexes.size(), values.size());

    RecordReader reader(tmp.path());
    for (std::size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(reader.read(indexes[i]), values[i]) << "record " << i;
    }
    EXPECT_EQ(test::drain(reader), values) << "a batch keeps the order it was given in";
}

TEST_F(ApiTest, AnEmptyBatchDoesNothing) {
    RecordWriter writer(tmp.path());
    EXPECT_TRUE(writer.appendBatch(std::vector<std::string>{}).empty());
    EXPECT_TRUE(test::drain(tmp.path()).empty());
}

// The whole point of the API: a call that returned is a record that is on
// disk, whether or not anything was closed afterwards.
TEST_F(ApiTest, RecordsAreOnDiskBeforeAppendReturns) {
    std::vector<INDEX_TYPE> indexes;
    {
        RecordWriter writer(tmp.path());
        for (int i = 0; i < 8; ++i) {
            indexes.push_back(writer.append(test::makePayload(0, i, 100)));
            // Read it back through a separate reader while the writer is still
            // open and has not been told to flush anything.
            RecordReader reader(tmp.path());
            EXPECT_EQ(reader.read(indexes.back()), test::makePayload(0, i, 100));
        }
    }
    RecordReader reader(tmp.path());
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        EXPECT_EQ(reader.read(indexes[i]), test::makePayload(0, i, 100));
    }
}

TEST_F(ApiTest, ReadNextWalksTheArchiveInOrder) {
    std::vector<std::string> expected;
    {
        RecordWriter writer(tmp.path());
        for (int i = 0; i < 20; ++i) {
            expected.push_back(test::makePayload(0, i, 64 + i));
            writer.append(expected.back());
        }
        writer.close();
    }
    RecordReader reader(tmp.path());
    EXPECT_EQ(test::drain(reader), expected);
}

TEST_F(ApiTest, ReadNextEndsWithNullopt) {
    RecordWriter writer(tmp.path());
    writer.append(std::string("only"));

    RecordReader reader(tmp.path());
    EXPECT_EQ(reader.readNext(), "only");
    EXPECT_EQ(reader.readNext(), std::nullopt);
    EXPECT_EQ(reader.readNext(), std::nullopt) << "asking again must stay quiet";
}

TEST_F(ApiTest, CurrentIndexNamesTheRecordReadNextReturned) {
    RecordWriter writer(tmp.path());
    const INDEX_TYPE first = writer.append(std::string("a"));
    const INDEX_TYPE second = writer.append(std::string("b"));

    RecordReader reader(tmp.path());
    EXPECT_EQ(reader.currentIndex(), kInvalidIndex);
    ASSERT_TRUE(reader.readNext().has_value());
    EXPECT_EQ(reader.currentIndex(), first);
    ASSERT_TRUE(reader.readNext().has_value());
    EXPECT_EQ(reader.currentIndex(), second);
    EXPECT_EQ(reader.read(reader.currentIndex()), "b");
}

TEST_F(ApiTest, RewindStartsTheWalkOver) {
    RecordWriter writer(tmp.path());
    writer.appendBatch(std::vector<std::string>{"a", "b", "c"});

    RecordReader reader(tmp.path());
    const std::vector<std::string> first = test::drain(reader);
    reader.rewind();
    EXPECT_EQ(reader.currentIndex(), kInvalidIndex);
    EXPECT_EQ(test::drain(reader), first);
}

TEST_F(ApiTest, PayloadsAreOpaqueBytes) {
    const std::string binary("a\0b\xff\x00 \n\t", 8);
    RecordWriter writer(tmp.path());
    const INDEX_TYPE index = writer.append(binary);

    RecordReader reader(tmp.path());
    const std::optional<std::string> value = reader.read(index);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->size(), 8u);
    EXPECT_EQ(*value, binary);
}

TEST_F(ApiTest, WritingAfterCloseIsAProgrammingError) {
    RecordWriter writer(tmp.path());
    writer.append(std::string("before"));
    writer.close();

    EXPECT_THROW(writer.append(std::string("after")), std::logic_error);
    EXPECT_THROW(writer.appendBatch(std::vector<std::string>{"after"}), std::logic_error);
    EXPECT_THROW(writer.segmentId(), std::logic_error);
    // What was written before the close is still there.
    EXPECT_EQ(test::drain(tmp.path()), (std::vector<std::string>{"before"}));
}

TEST_F(ApiTest, ReadingAfterCloseIsAProgrammingError) {
    RecordWriter writer(tmp.path());
    const INDEX_TYPE index = writer.append(std::string("value"));

    RecordReader reader(tmp.path());
    ASSERT_EQ(reader.read(index), "value");
    reader.close();

    EXPECT_THROW(reader.read(index), std::logic_error);
    EXPECT_THROW(reader.readNext(), std::logic_error);
    EXPECT_THROW(reader.rewind(), std::logic_error);
}

TEST_F(ApiTest, ClosingTwiceIsAllowed) {
    RecordWriter writer(tmp.path());
    writer.close();
    EXPECT_NO_THROW(writer.close());

    RecordReader reader(tmp.path());
    reader.close();
    EXPECT_NO_THROW(reader.close());
}

TEST_F(ApiTest, ClosingAWriterReleasesItsSegment) {
    RecordWriter first(tmp.path());
    const std::uint32_t id = first.segmentId();
    first.close();

    RecordWriter second(tmp.path());
    EXPECT_EQ(second.segmentId(), id) << "a released segment is available again";
}

TEST_F(ApiTest, DestroyingAWriterReleasesItsSegment) {
    std::uint32_t id = 0;
    {
        RecordWriter writer(tmp.path());
        id = writer.segmentId();
    }
    RecordWriter second(tmp.path());
    EXPECT_EQ(second.segmentId(), id);
}

TEST_F(ApiTest, WritersDoNotShareASegment) {
    RecordWriter first(tmp.path());
    RecordWriter second(tmp.path());
    RecordWriter third(tmp.path());
    EXPECT_NE(first.segmentId(), second.segmentId());
    EXPECT_NE(second.segmentId(), third.segmentId());
    EXPECT_NE(first.segmentId(), third.segmentId());
}

TEST_F(ApiTest, OpeningAnArchiveThatIsNotThereFails) {
    EXPECT_THROW(RecordReader reader(tmp.path()), std::system_error);
    EXPECT_THROW(RecordReader reader(tmp.path() + "/deeper"), std::system_error);
}

TEST_F(ApiTest, AnArchiveWithNoRecordsReadsAsEmpty) {
    { RecordWriter writer(tmp.path()); }
    RecordReader reader(tmp.path());
    EXPECT_EQ(reader.readNext(), std::nullopt);
    EXPECT_TRUE(test::drain(reader).empty());
}

TEST_F(ApiTest, AWriterCanReopenAnArchiveItAlreadyWrote) {
    std::vector<std::string> expected;
    for (int round = 0; round < 3; ++round) {
        RecordWriter writer(tmp.path());
        for (int i = 0; i < 4; ++i) {
            expected.push_back(test::makePayload(0, round * 4 + i, 48));
            writer.append(expected.back());
        }
        writer.close();
    }
    EXPECT_EQ(test::drain(tmp.path()), expected)
        << "reopening must append, never overwrite what is there";
}

TEST_F(ApiTest, ReadersAndWritersMoveWithoutLosingTheirArchive) {
    RecordWriter writer(tmp.path());
    writer.append(std::string("first"));
    RecordWriter moved = std::move(writer);
    const INDEX_TYPE index = moved.append(std::string("second"));

    RecordReader reader(tmp.path());
    RecordReader moved_reader = std::move(reader);
    EXPECT_EQ(moved_reader.read(index), "second");
    EXPECT_EQ(test::drain(moved_reader), (std::vector<std::string>{"first", "second"}));
}

TEST_F(ApiTest, StringViewOverloadsBehaveLikeTheStringOnes) {
    RecordWriter writer(tmp.path());
    const std::string backing = "abcdef";
    const INDEX_TYPE index = writer.append(std::string_view(backing).substr(1, 3));
    const std::vector<std::string_view> batch = {std::string_view(backing).substr(0, 2),
                                                 std::string_view(backing).substr(4)};
    const std::vector<INDEX_TYPE> indexes = writer.appendBatch(batch);

    RecordReader reader(tmp.path());
    EXPECT_EQ(reader.read(index), "bcd");
    EXPECT_EQ(reader.read(indexes[0]), "ab");
    EXPECT_EQ(reader.read(indexes[1]), "ef");
}

}  // namespace
}  // namespace raf
