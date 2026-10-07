// The short suite: the properties raf would be broken without.
//
// `raf_test` has 160 tests and covers the API's manners as well as its
// promises -- what a double close does, whether a move leaves the archive
// behind, which exception a path that is really a file throws. Those are
// worth having and they are not here. What is here is the set that, if any
// one of them failed, would mean a record was lost, duplicated, reordered,
// handed back wrong, or never there at all.
//
// One file, twenty-three tests, about three seconds. Run it when the long
// one is too slow to run; run the long one before shipping.
#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <latch>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <raf/raf.h>

#include "common/crc32c.h"
#include "store/format.h"
#include "store/segment.h"
#include "test_support.h"

namespace raf {
namespace {

constexpr std::size_t kSize = 256;
constexpr std::uint64_t kSpan = format::kMetaSize + kSize;

class CoreTest : public ::testing::Test {
protected:
    test::TempArchive tmp{"raf-core"};
    Options fast;  // durability has its own test; the rest move volume

    CoreTest() { fast.sync = false; }

    static std::vector<std::string> series(std::uint32_t writer, std::size_t count,
                                           std::size_t first = 0) {
        std::vector<std::string> values;
        for (std::size_t i = 0; i < count; ++i) {
            values.push_back(test::makePayload(writer, first + i, kSize));
        }
        return values;
    }

    // Every record intact, each writer's own records all present and in that
    // writer's order. No order is promised between writers, so none is
    // checked.
    void expectExactlyOnce(std::uint32_t writers, std::size_t per_writer) {
        const std::vector<std::string> values = test::drain(tmp.path());
        EXPECT_EQ(values.size(), writers * per_writer);

        std::map<std::uint32_t, std::vector<std::uint64_t>> seen;
        for (const std::string& value : values) {
            ASSERT_TRUE(test::payloadIsIntact(value))
                << "a record came back that nobody wrote in one piece";
            seen[test::identifyPayload(value)->writer].push_back(
                test::identifyPayload(value)->sequence);
        }
        EXPECT_EQ(seen.size(), writers);
        for (std::uint32_t w = 1; w <= writers; ++w) {
            std::vector<std::uint64_t> want(per_writer);
            for (std::size_t i = 0; i < per_writer; ++i) {
                want[i] = i;
            }
            EXPECT_EQ(seen[w], want) << "writer " << w << " lost, duplicated or reordered records";
        }
    }
};

// ---------------------------------------------------------------- durability

// The promise the whole API is built on: when append returns, the record is
// on disk. A second reader opened after the fact is the only way to ask that
// does not trust the writer's own memory of it.
TEST_F(CoreTest, ARecordIsOnDiskBeforeAppendReturns) {
    RecordWriter writer(tmp.path());  // sync on, which is the default
    for (std::size_t i = 0; i < 20; ++i) {
        const std::string value = test::makePayload(1, i, kSize);
        const INDEX_TYPE index = writer.append(value);
        EXPECT_EQ(RecordReader(tmp.path()).read(index), value) << "record " << i;
    }
}

TEST_F(CoreTest, AnIndexReadsBackTheRecordItNamed) {
    RecordWriter writer(tmp.path(), fast);
    std::vector<INDEX_TYPE> indexes;
    const std::vector<std::string> values = series(1, 200);
    for (const std::string& value : values) {
        indexes.push_back(writer.append(value));
    }
    writer.close();

    RecordReader reader(tmp.path());
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        EXPECT_EQ(reader.read(indexes[i]), values[i]) << "record " << i;
    }
}

TEST_F(CoreTest, ABatchIsAllOfItOrNoneOfIt) {
    RecordWriter writer(tmp.path(), fast);
    const std::vector<std::string> values = series(1, 50);
    const std::vector<INDEX_TYPE> indexes = writer.appendBatch(values);
    ASSERT_EQ(indexes.size(), values.size());

    // One value over the limit rejects the batch; nothing from it lands.
    std::vector<std::string> bad = series(2, 3);
    bad.push_back(std::string(kMaxRecordSize + 1, 'x'));
    EXPECT_THROW(writer.appendBatch(bad), std::logic_error);
    writer.close();

    EXPECT_EQ(test::drain(tmp.path()), values);
}

// ----------------------------------------------------------------- iteration

TEST_F(CoreTest, OneWriterComesBackInAppendOrder) {
    const std::vector<std::string> values = series(1, 300);
    {
        RecordWriter writer(tmp.path(), fast);
        for (const std::string& value : values) {
            writer.append(value);
        }
    }
    EXPECT_EQ(test::drain(tmp.path()), values);
}

// Two readers that opened on the same segment tails walk what was already
// there in the same order. This is the half of the iteration contract that
// is a total order; the other half is only per writer.
TEST_F(CoreTest, ReadersAgreeOnWhatWasThereWhenTheyOpened) {
    {
        RecordWriter first(tmp.path(), fast);
        RecordWriter second(tmp.path(), fast);
        for (std::size_t i = 0; i < 100; ++i) {
            first.append(test::makePayload(1, i, kSize));
            second.append(test::makePayload(2, i, kSize));
        }
    }
    RecordReader a(tmp.path());
    RecordReader b(tmp.path());
    EXPECT_EQ(test::drain(a), test::drain(b));
}

// A reader that ran out has to look again rather than give up for good --
// both at records appended to a segment it knows and at segments that did
// not exist when it opened.
TEST_F(CoreTest, IterationPicksUpWhatWasWrittenAfterItCaughtUp) {
    RecordWriter first(tmp.path(), fast);
    first.append(test::makePayload(1, 0, kSize));

    RecordReader reader(tmp.path());
    ASSERT_TRUE(reader.readNext().has_value());
    ASSERT_EQ(reader.readNext(), std::nullopt);

    first.append(test::makePayload(1, 1, kSize));
    EXPECT_EQ(reader.readNext(), test::makePayload(1, 1, kSize));

    RecordWriter second(tmp.path(), fast);
    ASSERT_NE(second.segmentId(), first.segmentId());
    second.append(test::makePayload(2, 0, kSize));
    EXPECT_EQ(reader.readNext(), test::makePayload(2, 0, kSize));
}

// A writer still in flight stalls its own segment, not the walk. Returning
// end-of-archive here would hide a finished record in another segment behind
// an append that has not returned.
TEST_F(CoreTest, ARecordStillBeingWrittenDoesNotHideAnotherSegmentsRecords) {
    const std::string finished = test::makePayload(1, 0, kSize);
    const std::string elsewhere = test::makePayload(2, 0, kSize);

    // The writer stays open: a closed segment is free for the next writer to
    // take and recover, and recovery would cut the torn header off first.
    RecordWriter first(tmp.path(), fast);
    first.append(finished);

    format::RecordMeta in_flight{};
    in_flight.magic = format::kRecordMagic;
    in_flight.length = kSize;
    in_flight.timestamp_ns = 1;  // older than anything appended below
    in_flight.sequence = 1;
    in_flight.data_crc = 0;
    in_flight.meta_crc = format::computeMetaCrc(in_flight);
    test::writeRaw(test::segmentPaths(tmp.path()).front(), format::kHeaderSize + kSpan, &in_flight,
                   sizeof(in_flight));

    RecordReader reader(tmp.path());
    ASSERT_EQ(reader.readNext(), finished);

    RecordWriter second(tmp.path(), fast);
    second.append(elsewhere);
    EXPECT_EQ(reader.readNext(), elsewhere);
}

// ------------------------------------------------------------------ extremes

TEST_F(CoreTest, EmptyAndLargestRecordsRoundTrip) {
    const std::string largest = test::makePayload(1, 0, kMaxRecordSize);
    INDEX_TYPE empty_index = kInvalidIndex;
    INDEX_TYPE largest_index = kInvalidIndex;
    {
        RecordWriter writer(tmp.path(), fast);
        empty_index = writer.append(std::string());
        largest_index = writer.append(largest);
    }
    RecordReader reader(tmp.path());
    // An empty record is a record, not "nothing is there", which is what
    // nullopt means.
    EXPECT_EQ(reader.read(empty_index), std::string());
    EXPECT_EQ(reader.read(largest_index), largest);
    EXPECT_EQ(test::drain(tmp.path()).size(), 2u);
}

TEST_F(CoreTest, ARecordOverTheLimitIsRefusedBeforeAnythingIsWritten) {
    RecordWriter writer(tmp.path(), fast);
    const INDEX_TYPE before = writer.append(test::makePayload(1, 0, kSize));
    EXPECT_THROW(writer.append(std::string(kMaxRecordSize + 1, 'x')), std::logic_error);
    writer.close();

    EXPECT_EQ(test::drain(tmp.path()), std::vector<std::string>{test::makePayload(1, 0, kSize)});
    EXPECT_EQ(RecordReader(tmp.path()).read(before), test::makePayload(1, 0, kSize));
}

TEST_F(CoreTest, RollingOverToANewSegmentKeepsTheWalkInOrder) {
    Options small = fast;
    small.maxSegmentBytes = format::kHeaderSize + 10 * kSpan;
    const std::vector<std::string> values = series(1, 95);
    {
        RecordWriter writer(tmp.path(), small);
        for (const std::string& value : values) {
            writer.append(value);
        }
    }
    EXPECT_GT(test::segmentPaths(tmp.path()).size(), 1u) << "the limit should have forced a roll";
    EXPECT_EQ(test::drain(tmp.path()), values) << "one writer keeps its order across segments";
}

// -------------------------------------------------------------------- damage

TEST_F(CoreTest, ADamagedPayloadIsNeverReturned) {
    std::vector<INDEX_TYPE> indexes;
    const std::vector<std::string> values = series(1, 4);
    {
        RecordWriter writer(tmp.path(), fast);
        for (const std::string& value : values) {
            indexes.push_back(writer.append(value));
        }
    }
    test::flipBit(test::segmentPaths(tmp.path()).front(), offsetOf(indexes[1]) + format::kMetaSize);

    RecordReader reader(tmp.path());
    EXPECT_EQ(reader.read(indexes[1]), std::nullopt) << "a payload that fails its checksum is lost";
    EXPECT_EQ(reader.read(indexes[0]), values[0]);
    EXPECT_EQ(reader.read(indexes[2]), values[2]) << "and costs the archive nothing else";

    std::vector<std::string> rest = values;
    rest.erase(rest.begin() + 1);
    EXPECT_EQ(test::drain(tmp.path()), rest) << "the walk steps over it";
}

// A header that fails its checksum below the frontier makes the next
// record's offset unknowable, so the walk stops there rather than guessing
// where the next one starts.
TEST_F(CoreTest, ADamagedHeaderStopsIterationRatherThanGuessing) {
    std::vector<INDEX_TYPE> indexes;
    const std::vector<std::string> values = series(1, 5);
    {
        RecordWriter writer(tmp.path(), fast);
        for (const std::string& value : values) {
            indexes.push_back(writer.append(value));
        }
    }
    test::flipBit(test::segmentPaths(tmp.path()).front(), offsetOf(indexes[2]));

    EXPECT_EQ(test::drain(tmp.path()),
              std::vector<std::string>(values.begin(), values.begin() + 2));
}

TEST_F(CoreTest, DamageInOneSegmentDoesNotStopTheOthers) {
    std::vector<INDEX_TYPE> first_indexes;
    {
        RecordWriter first(tmp.path(), fast);
        RecordWriter second(tmp.path(), fast);
        for (std::size_t i = 0; i < 20; ++i) {
            first_indexes.push_back(first.append(test::makePayload(1, i, kSize)));
            second.append(test::makePayload(2, i, kSize));
        }
    }
    const std::vector<std::string> paths = test::segmentPaths(tmp.path());
    ASSERT_EQ(paths.size(), 2u);
    test::flipBit(paths.front(), offsetOf(first_indexes[0]));

    std::size_t from_second = 0;
    for (const std::string& value : test::drain(tmp.path())) {
        ASSERT_TRUE(test::payloadIsIntact(value));
        if (test::identifyPayload(value)->writer == 2) {
            ++from_second;
        }
    }
    EXPECT_EQ(from_second, 20u) << "a broken segment must not cost the other one its records";
}

// ------------------------------------------------------------------ recovery

TEST_F(CoreTest, RecordsSurviveSigkill) {
    ASSERT_TRUE(test::killedBy(test::runInChild([&] {
                                   RecordWriter writer(tmp.path());
                                   for (std::size_t i = 0; i < 16; ++i) {
                                       writer.append(test::makePayload(1, i, kSize));
                                   }
                                   ::raise(SIGKILL);
                                   return 0;
                               }),
                               SIGKILL));
    EXPECT_EQ(test::drain(tmp.path()), series(1, 16));
}

// A crash can stop a write anywhere, so recovery is checked at every
// interesting byte rather than at a few chosen stages: on a boundary, one
// byte in, inside a header, inside a payload, one byte short.
TEST_F(CoreTest, AFileCutAtAnyPointRecoversToARecordBoundary) {
    {
        RecordWriter writer(tmp.path());
        for (const std::string& value : series(1, 6)) {
            writer.append(value);
        }
    }
    const std::string path = test::segmentPaths(tmp.path()).front();
    const std::string original = test::readRaw(path, 0, format::kHeaderSize + 6 * kSpan);
    ASSERT_EQ(original.size(), format::kHeaderSize + 6 * kSpan);

    for (const std::uint64_t cut : {format::kHeaderSize, format::kHeaderSize + 1,
                                   format::kHeaderSize + format::kMetaSize,
                                   format::kHeaderSize + kSpan / 2, format::kHeaderSize + kSpan - 1,
                                   format::kHeaderSize + kSpan, format::kHeaderSize + 3 * kSpan,
                                   format::kHeaderSize + 6 * kSpan}) {
        test::writeRaw(path, 0, original.data(), original.size());
        test::truncateFile(path, cut);
        const std::size_t complete = static_cast<std::size_t>((cut - format::kHeaderSize) / kSpan);

        EXPECT_EQ(test::drain(tmp.path()), series(1, complete)) << "cut at " << cut;

        // Adopting the segment trims it there and then, rather than leaving
        // the debris for the next append to happen to cover.
        { RecordWriter adopt(tmp.path()); }
        EXPECT_EQ(test::fileSize(path), format::kHeaderSize + complete * kSpan)
            << "cut at " << cut;

        // And the next writer appends from that boundary, so no record can
        // land in a hole left by the one that was cut.
        {
            RecordWriter writer(tmp.path());
            writer.append(test::makePayload(9, 0, kSize));
        }
        EXPECT_EQ(test::fileSize(path), format::kHeaderSize + (complete + 1) * kSpan)
            << "cut at " << cut;
        std::vector<std::string> after = series(1, complete);
        after.push_back(test::makePayload(9, 0, kSize));
        EXPECT_EQ(test::drain(tmp.path()), after) << "cut at " << cut;
    }
}

TEST_F(CoreTest, RecordsWrittenAfterRecoveryDoNotOverwriteTheOldOnes) {
    ASSERT_TRUE(test::exitedWith(test::runInChild([&] {
                                     RecordWriter writer(tmp.path());
                                     for (std::size_t i = 0; i < 8; ++i) {
                                         writer.append(test::makePayload(1, i, kSize));
                                     }
                                     return 0;  // no close, no destructor
                                 }),
                                 0));
    {
        RecordWriter writer(tmp.path());
        for (std::size_t i = 8; i < 16; ++i) {
            writer.append(test::makePayload(1, i, kSize));
        }
    }
    EXPECT_EQ(test::drain(tmp.path()), series(1, 16));
}

// --------------------------------------------------------------- concurrency

// The arrangement the design settles on: one writer per thread, each with its
// own segment, nobody waiting on anybody.
TEST_F(CoreTest, AWriterPerThread) {
    constexpr std::uint32_t kThreads = 8;
    constexpr std::size_t kPerThread = 200;

    std::latch start(1);
    std::vector<std::thread> threads;
    for (std::uint32_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            RecordWriter writer(tmp.path(), fast);
            start.wait();
            for (std::size_t i = 0; i < kPerThread; ++i) {
                writer.append(test::makePayload(t + 1, i, kSize));
            }
        });
    }
    start.count_down();
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(test::segmentPaths(tmp.path()).size(), kThreads);
    expectExactlyOnce(kThreads, kPerThread);
}

// Iteration state belongs to the reader, not the thread: a shared reader
// hands each record to exactly one caller.
TEST_F(CoreTest, ReadNextFromManyThreadsHandsOutEachRecordOnce) {
    constexpr std::size_t kRecords = 600;
    {
        RecordWriter writer(tmp.path(), fast);
        for (std::size_t i = 0; i < kRecords; ++i) {
            writer.append(test::makePayload(1, i, kSize));
        }
    }

    RecordReader reader(tmp.path());
    std::latch start(1);
    std::mutex mutex;
    std::multiset<std::uint64_t> seen;
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&] {
            start.wait();
            while (std::optional<std::string> value = reader.readNext()) {
                ASSERT_TRUE(test::payloadIsIntact(*value));
                const std::uint64_t sequence = test::identifyPayload(*value)->sequence;
                std::lock_guard<std::mutex> guard(mutex);
                seen.insert(sequence);
            }
        });
    }
    start.count_down();
    for (std::thread& thread : threads) {
        thread.join();
    }

    ASSERT_EQ(seen.size(), kRecords);
    for (std::uint64_t i = 0; i < kRecords; ++i) {
        EXPECT_EQ(seen.count(i), 1u) << "record " << i << " was handed out twice or not at all";
    }
}

// Readers walking an archive several writers are still appending to. This is
// the test that caught an O_DIRECT read going unaligned off the end of a
// file that was growing underneath it.
TEST_F(CoreTest, ReadersAndWritersRunTogether) {
    constexpr std::uint32_t kWriters = 4;
    constexpr std::size_t kPerWriter = 250;

    std::latch start(1);
    std::atomic<bool> writing{true};
    std::atomic<int> broken{0};
    std::atomic<std::size_t> records_seen{0};

    std::vector<std::thread> threads;
    for (std::uint32_t w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            RecordWriter writer(tmp.path(), fast);
            start.wait();
            for (std::size_t i = 0; i < kPerWriter; ++i) {
                writer.append(test::makePayload(w + 1, i, 180));
            }
            writer.close();
        });
    }
    for (int r = 0; r < 3; ++r) {
        threads.emplace_back([&] {
            start.wait();
            RecordReader reader(tmp.path());
            std::size_t count = 0;
            while (writing.load() || count == 0) {
                std::optional<std::string> value = reader.readNext();
                if (!value) {
                    if (!writing.load()) {
                        break;
                    }
                    continue;
                }
                ++count;
                if (!test::payloadIsIntact(*value)) {
                    ++broken;
                }
            }
            records_seen += count;
        });
    }
    start.count_down();
    for (std::uint32_t w = 0; w < kWriters; ++w) {
        threads[w].join();
    }
    writing.store(false);
    for (std::size_t i = kWriters; i < threads.size(); ++i) {
        threads[i].join();
    }

    EXPECT_EQ(broken.load(), 0) << "a reader saw a record that was not fully written";
    EXPECT_GT(records_seen.load(), 0u);
}

// -------------------------------------------------------------- multiprocess

// A writer's claim on a segment is an flock, which conflicts between open
// file descriptions -- so the mechanism that separates two threads separates
// two processes, with no extra code.
TEST_F(CoreTest, ProcessesNeverShareASegment) {
    constexpr int kChildren = 4;
    constexpr std::size_t kPerChild = 100;
    { RecordWriter warmup(tmp.path()); }  // so the children do not race to create it

    std::vector<int> pids;
    for (int c = 0; c < kChildren; ++c) {
        pids.push_back(test::runInChild([&, c] {
            RecordWriter writer(tmp.path());
            for (std::size_t i = 0; i < kPerChild; ++i) {
                writer.append(test::makePayload(static_cast<std::uint32_t>(c + 1), i, kSize));
            }
            writer.close();
            return 0;
        }));
    }
    for (const int status : pids) {
        ASSERT_TRUE(test::exitedWith(status, 0));
    }
    expectExactlyOnce(kChildren, kPerChild);
}

// The kernel drops a dead process's flock, so the next writer adopts the
// segment, recovers it, and appends from where the dead one stopped.
TEST_F(CoreTest, ASegmentHeldByADeadProcessIsTakenOver) {
    ASSERT_TRUE(test::killedBy(test::runInChild([&] {
                                   RecordWriter writer(tmp.path());
                                   for (std::size_t i = 0; i < 6; ++i) {
                                       writer.append(test::makePayload(1, i, kSize));
                                   }
                                   ::raise(SIGKILL);
                                   return 0;
                               }),
                               SIGKILL));
    ASSERT_EQ(test::segmentPaths(tmp.path()).size(), 1u);

    {
        RecordWriter writer(tmp.path());
        EXPECT_EQ(writer.segmentId(), 0u) << "the freed segment, not a new one";
        writer.append(test::makePayload(2, 0, kSize));
    }
    EXPECT_EQ(test::segmentPaths(tmp.path()).size(), 1u);

    std::vector<std::string> expected = series(1, 6);
    expected.push_back(test::makePayload(2, 0, kSize));
    EXPECT_EQ(test::drain(tmp.path()), expected);
}

// ------------------------------------------------------------------ checksum

// Every verdict about damage rests on this. The fast path folds three
// interleaved checksums together with precomputed GF(2) shifts, which can be
// subtly wrong while still looking plausible, so it is checked against the
// definition a bit at a time.
TEST(CoreChecksum, Crc32cMatchesItsDefinitionAtEveryLength) {
    const auto reference = [](const void* data, std::size_t length) {
        const auto* p = static_cast<const unsigned char*>(data);
        std::uint32_t crc = 0xffffffffu;
        for (std::size_t i = 0; i < length; ++i) {
            crc ^= p[i];
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ (0x82f63b78u & (~(crc & 1u) + 1u));
            }
        }
        return ~crc;
    };

    std::vector<unsigned char> data(1100);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<unsigned char>((i * 31u) ^ 0x5au);
    }
    // Every length through the word loop, its tail, and the seam where the
    // three interleaved checksums are folded back together.
    for (std::size_t n = 0; n <= data.size(); ++n) {
        EXPECT_EQ(crc32c(data.data(), n), reference(data.data(), n)) << "length " << n;
    }
    EXPECT_EQ(crc32c("123456789", 9), 0xe3069283u) << "the published check value";
}

// -------------------------------------------------------------------- direct

// Reads go through O_DIRECT by default, which means no page cache and three
// alignment rules -- offset, length and buffer address all multiples of the
// device's logical block. A raf record obeys none of them on its own, so
// store::ReadWindow is what makes the reads legal. These sizes are multiples
// of nothing, so every record starts and ends mid-sector.
TEST_F(CoreTest, IterationAndRandomLookupAgreeAtRaggedSizes) {
    std::vector<INDEX_TYPE> indexes;
    std::vector<std::string> values;
    {
        RecordWriter writer(tmp.path(), fast);
        for (int i = 0; i < 400; ++i) {
            values.push_back(test::makePayload(1, i, 37 + (i * 97) % 9000));
            indexes.push_back(writer.append(values.back()));
        }
    }
    EXPECT_EQ(test::drain(tmp.path()), values);

    RecordReader reader(tmp.path());
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        EXPECT_EQ(reader.read(indexes[i]), values[i]) << "record " << i;
    }
}

// A record larger than the window still has to come back whole.
TEST_F(CoreTest, ARecordLargerThanTheWindowStillReadsBack) {
    const std::string big = test::makePayload(1, 0, 3 * store::ReadWindow::kDefaultCapacity);
    INDEX_TYPE index = kInvalidIndex;
    {
        RecordWriter writer(tmp.path(), fast);
        writer.append(test::makePayload(1, 1, kSize));
        index = writer.append(big);
        writer.append(test::makePayload(1, 2, kSize));
    }
    EXPECT_EQ(RecordReader(tmp.path()).read(index), big);
    EXPECT_EQ(test::drain(tmp.path()).size(), 3u) << "and the records around it still walk";
}

}  // namespace
}  // namespace raf
