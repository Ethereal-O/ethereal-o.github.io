// The iteration contract.
//
//   * Records that were already complete when a reader opened come out in one
//     order, the same for every reader that opened on the same segment tails.
//   * Records appended after that keep the order of the writer that wrote
//     them; no order is defined between writers.
#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <raf/raf.h>

#include "store/format.h"
#include "test_support.h"

namespace raf {
namespace {

class IterationTest : public ::testing::Test {
protected:
    test::TempArchive tmp{"raf-iteration"};
    Options fast;

    IterationTest() { fast.sync = false; }

    // Identities of the records in `values`, in the order they came out.
    static std::vector<test::PayloadId> identify(const std::vector<std::string>& values) {
        std::vector<test::PayloadId> ids;
        for (const std::string& value : values) {
            EXPECT_TRUE(test::payloadIsIntact(value));
            ids.push_back(*test::identifyPayload(value));
        }
        return ids;
    }

    // Sequence numbers each writer's records appeared in, in output order.
    static std::map<std::uint32_t, std::vector<std::uint64_t>> perWriter(
        const std::vector<test::PayloadId>& ids) {
        std::map<std::uint32_t, std::vector<std::uint64_t>> by_writer;
        for (const test::PayloadId& id : ids) {
            by_writer[id.writer].push_back(id.sequence);
        }
        return by_writer;
    }

    static void expectAscending(const std::vector<std::uint64_t>& sequences) {
        for (std::size_t i = 1; i < sequences.size(); ++i) {
            EXPECT_LT(sequences[i - 1], sequences[i]) << "a writer's records came out reordered";
        }
    }
};

TEST_F(IterationTest, OneWriterComesBackInAppendOrder) {
    std::vector<std::string> expected;
    RecordWriter writer(tmp.path(), fast);
    for (int i = 0; i < 50; ++i) {
        expected.push_back(test::makePayload(0, i, 80));
        writer.append(expected.back());
    }
    EXPECT_EQ(test::drain(tmp.path()), expected);
}

// The worked example from the design doc: two writers interleaved, two readers
// opened between two rounds of appends.
TEST_F(IterationTest, ReadersAgreeOnWhatWasThereWhenTheyOpened) {
    RecordWriter first(tmp.path(), fast);
    RecordWriter second(tmp.path(), fast);
    for (int i = 0; i < 2; ++i) {
        first.append(test::makePayload(1, i, 64));
        second.append(test::makePayload(2, i, 64));
    }

    RecordReader reader_a(tmp.path());
    RecordReader reader_b(tmp.path());

    for (int i = 2; i < 4; ++i) {
        first.append(test::makePayload(1, i, 64));
        second.append(test::makePayload(2, i, 64));
    }

    const std::vector<test::PayloadId> a = identify(test::drain(reader_a));
    const std::vector<test::PayloadId> b = identify(test::drain(reader_b));
    ASSERT_EQ(a.size(), 8u);
    ASSERT_EQ(b.size(), 8u);

    // The four records that existed when both readers opened come first, in
    // the same order for both.
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(a[i].writer, b[i].writer) << "position " << i;
        EXPECT_EQ(a[i].sequence, b[i].sequence) << "position " << i;
        EXPECT_LT(a[i].sequence, 2u) << "a later record jumped ahead of the ones already on disk";
    }

    // For the rest, only each writer's own order is promised.
    for (const std::vector<test::PayloadId>& order : {a, b}) {
        for (const auto& [writer, sequences] : perWriter(order)) {
            EXPECT_EQ(sequences.size(), 4u) << "writer " << writer;
            expectAscending(sequences);
        }
    }
}

TEST_F(IterationTest, TheOrderOfWhatWasAlreadyThereDoesNotDependOnWhenYouRead) {
    {
        RecordWriter first(tmp.path(), fast);
        RecordWriter second(tmp.path(), fast);
        RecordWriter third(tmp.path(), fast);
        for (int i = 0; i < 10; ++i) {
            first.append(test::makePayload(1, i, 48));
            second.append(test::makePayload(2, i, 48));
            third.append(test::makePayload(3, i, 48));
        }
    }
    const std::vector<test::PayloadId> reference = identify(test::drain(tmp.path()));
    ASSERT_EQ(reference.size(), 30u);
    for (int repeat = 0; repeat < 3; ++repeat) {
        const std::vector<test::PayloadId> again = identify(test::drain(tmp.path()));
        ASSERT_EQ(again.size(), reference.size());
        for (std::size_t i = 0; i < reference.size(); ++i) {
            EXPECT_EQ(again[i].writer, reference[i].writer) << "repeat " << repeat << " at " << i;
            EXPECT_EQ(again[i].sequence, reference[i].sequence)
                << "repeat " << repeat << " at " << i;
        }
    }
}

TEST_F(IterationTest, ARecordAppendedAfterTheReaderCaughtUpBecomesVisible) {
    RecordWriter writer(tmp.path(), fast);
    writer.append(test::makePayload(0, 0, 64));

    RecordReader reader(tmp.path());
    ASSERT_TRUE(reader.readNext().has_value());
    ASSERT_EQ(reader.readNext(), std::nullopt);

    writer.append(test::makePayload(0, 1, 64));
    EXPECT_EQ(reader.readNext(), test::makePayload(0, 1, 64))
        << "a reader that ran out has to look again, not give up for good";
    EXPECT_EQ(reader.readNext(), std::nullopt);

    writer.appendBatch(
        std::vector<std::string>{test::makePayload(0, 2, 64), test::makePayload(0, 3, 64)});
    EXPECT_EQ(reader.readNext(), test::makePayload(0, 2, 64));
    EXPECT_EQ(reader.readNext(), test::makePayload(0, 3, 64));
}

TEST_F(IterationTest, AWriterThatStartedAfterTheReaderBecomesVisible) {
    RecordWriter first(tmp.path(), fast);
    first.append(test::makePayload(1, 0, 64));

    RecordReader reader(tmp.path());
    ASSERT_EQ(test::drain(reader).size(), 1u);

    // A second writer takes a segment that did not exist when the reader
    // opened.
    RecordWriter second(tmp.path(), fast);
    ASSERT_NE(second.segmentId(), first.segmentId());
    second.append(test::makePayload(2, 0, 64));

    const std::optional<std::string> value = reader.readNext();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(test::identifyPayload(*value)->writer, 2u);
}

// A writer still in flight must not hide another writer's finished records.
//
// One segment holds a header whose payload has not landed -- an append that
// has not returned yet, which is what a torn tail looks like from a reader's
// side. It is also the oldest thing left unread, so the merge reaches it
// first. Stopping there would report the end of the archive while a complete
// record sits in the next segment, which is the difference between "nothing
// more for now" and "nothing more until that writer finishes".
TEST_F(IterationTest, ARecordStillBeingWrittenDoesNotHideAnotherSegmentsRecords) {
    const std::string finished = test::makePayload(1, 0, 64);
    const std::string elsewhere = test::makePayload(2, 0, 64);

    // The first writer stays open: a closed segment is free for the next
    // writer to take and recover, and recovery would cut the torn header off
    // before the reader ever saw it.
    RecordWriter first(tmp.path(), fast);
    first.append(finished);
    const std::uint64_t torn_at = format::kHeaderSize + format::recordSpan(finished.size());

    // A header with no payload behind it, stamped older than anything the
    // second writer will append, so the merge prefers it.
    format::RecordMeta in_flight{};
    in_flight.magic = format::kRecordMagic;
    in_flight.length = 64;
    in_flight.timestamp_ns = 1;
    in_flight.sequence = 1;
    in_flight.data_crc = 0;
    in_flight.meta_crc = format::computeMetaCrc(in_flight);
    test::writeRaw(test::segmentPaths(tmp.path()).front(), torn_at, &in_flight,
                   sizeof(in_flight));

    RecordReader reader(tmp.path());
    ASSERT_EQ(reader.readNext(), finished);

    // A second writer, on a segment that did not exist when the reader
    // opened, finishes a record while the first one is still unfinished.
    RecordWriter second(tmp.path(), fast);
    second.append(elsewhere);

    EXPECT_EQ(reader.readNext(), elsewhere)
        << "the unfinished record stalls its own segment, not the walk";
}

TEST_F(IterationTest, RandomReadsAgreeWithTheWalk) {
    {
        RecordWriter first(tmp.path(), fast);
        RecordWriter second(tmp.path(), fast);
        for (int i = 0; i < 12; ++i) {
            first.append(test::makePayload(1, i, 100 + i));
            second.append(test::makePayload(2, i, 100 + i));
        }
    }
    RecordReader reader(tmp.path());
    RecordReader lookup(tmp.path());
    std::size_t count = 0;
    while (const std::optional<std::string> value = reader.readNext()) {
        const INDEX_TYPE index = reader.currentIndex();
        EXPECT_EQ(lookup.read(index), *value) << "record " << count;
        ++count;
    }
    EXPECT_EQ(count, 24u);
}

TEST_F(IterationTest, RewindReplaysTheSameRecords) {
    {
        RecordWriter first(tmp.path(), fast);
        RecordWriter second(tmp.path(), fast);
        for (int i = 0; i < 6; ++i) {
            first.append(test::makePayload(1, i, 64));
            second.append(test::makePayload(2, i, 64));
        }
    }
    RecordReader reader(tmp.path());
    const std::vector<std::string> first_pass = test::drain(reader);
    ASSERT_EQ(first_pass.size(), 12u);
    reader.rewind();
    EXPECT_EQ(test::drain(reader), first_pass);
}

TEST_F(IterationTest, RewindPicksUpWhatWasWrittenSinceTheReaderOpened) {
    RecordWriter writer(tmp.path(), fast);
    writer.append(test::makePayload(0, 0, 64));

    RecordReader reader(tmp.path());
    ASSERT_EQ(test::drain(reader).size(), 1u);

    writer.append(test::makePayload(0, 1, 64));
    reader.rewind();
    EXPECT_EQ(test::drain(reader).size(), 2u);
}

TEST_F(IterationTest, EmptyRecordsTakePartInTheWalk) {
    {
        RecordWriter writer(tmp.path(), fast);
        writer.appendBatch(std::vector<std::string>{"", "a", "", "", "b", ""});
    }
    EXPECT_EQ(test::drain(tmp.path()), (std::vector<std::string>{"", "a", "", "", "b", ""}));
}

TEST_F(IterationTest, SegmentRolloverKeepsTheWalkInOrder) {
    Options small = fast;
    small.maxSegmentBytes = format::kHeaderSize + 400;
    std::vector<std::string> expected;
    {
        RecordWriter writer(tmp.path(), small);
        for (int i = 0; i < 30; ++i) {
            expected.push_back(test::makePayload(0, i, 64));
            writer.append(expected.back());
        }
    }
    ASSERT_GT(test::segmentPaths(tmp.path()).size(), 3u);
    // One writer's records keep their order even when they are spread over
    // many segments.
    EXPECT_EQ(test::drain(tmp.path()), expected);
}

}  // namespace
}  // namespace raf
