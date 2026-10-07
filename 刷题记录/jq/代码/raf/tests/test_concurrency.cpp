// Threads.
//
// Every rendezvous here is a std::latch and every payload is a function of
// (writer, sequence): no sleeps, no random data, so a failure reproduces.
// What varies between runs is the interleaving, and none of the assertions
// depend on it -- they check the properties the contract actually promises.
#include <gtest/gtest.h>

#include <atomic>
#include <latch>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <raf/raf.h>

#include "test_support.h"

namespace raf {
namespace {

class ConcurrencyTest : public ::testing::Test {
protected:
    test::TempArchive tmp{"raf-concurrency"};
    Options fast;

    ConcurrencyTest() { fast.sync = false; }

    // Checks that an archive holds exactly `per_writer` records from each of
    // writers 1..writers, every one of them intact and in that writer's own
    // order.
    void expectExactlyOnce(std::uint32_t writers, std::size_t per_writer) {
        const std::vector<std::string> values = test::drain(tmp.path());
        EXPECT_EQ(values.size(), writers * per_writer);

        std::map<std::uint32_t, std::vector<std::uint64_t>> seen;
        for (const std::string& value : values) {
            ASSERT_TRUE(test::payloadIsIntact(value))
                << "a record came back that nobody wrote in one piece";
            const test::PayloadId id = *test::identifyPayload(value);
            seen[id.writer].push_back(id.sequence);
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

// The arrangement the design doc settles on: one writer per thread, each with
// its own segment, nobody waiting on anybody.
TEST_F(ConcurrencyTest, AWriterPerThread) {
    constexpr std::uint32_t kThreads = 8;
    constexpr std::size_t kPerThread = 200;

    std::latch start(1);
    std::vector<std::thread> threads;
    std::vector<std::vector<INDEX_TYPE>> indexes(kThreads);
    for (std::uint32_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            RecordWriter writer(tmp.path(), fast);
            start.wait();
            for (std::size_t i = 0; i < kPerThread; ++i) {
                indexes[t].push_back(writer.append(test::makePayload(t + 1, i, 128)));
            }
            writer.close();
        });
    }
    start.count_down();
    for (std::thread& thread : threads) {
        thread.join();
    }

    expectExactlyOnce(kThreads, kPerThread);

    // No two writers were handed the same place to write.
    std::set<INDEX_TYPE> all;
    for (const std::vector<INDEX_TYPE>& per_thread : indexes) {
        for (const INDEX_TYPE index : per_thread) {
            EXPECT_TRUE(all.insert(index).second) << "index " << index << " was handed out twice";
        }
    }
    EXPECT_EQ(all.size(), kThreads * kPerThread);

    RecordReader reader(tmp.path());
    for (std::uint32_t t = 0; t < kThreads; ++t) {
        for (std::size_t i = 0; i < kPerThread; ++i) {
            EXPECT_EQ(reader.read(indexes[t][i]), test::makePayload(t + 1, i, 128));
        }
    }
}

// One writer object shared by several threads. They serialize on it, which is
// slower than a writer each, but it has to be correct.
TEST_F(ConcurrencyTest, OneWriterSharedByManyThreads) {
    constexpr std::uint32_t kThreads = 6;
    constexpr std::size_t kPerThread = 150;

    RecordWriter writer(tmp.path(), fast);
    std::latch start(1);
    std::vector<std::thread> threads;
    std::vector<std::vector<INDEX_TYPE>> indexes(kThreads);
    for (std::uint32_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            start.wait();
            for (std::size_t i = 0; i < kPerThread; ++i) {
                indexes[t].push_back(writer.append(test::makePayload(t + 1, i, 96)));
            }
        });
    }
    start.count_down();
    for (std::thread& thread : threads) {
        thread.join();
    }
    writer.close();

    EXPECT_EQ(test::segmentPaths(tmp.path()).size(), 1u)
        << "one writer means one segment however many threads use it";
    expectExactlyOnce(kThreads, kPerThread);

    RecordReader reader(tmp.path());
    for (std::uint32_t t = 0; t < kThreads; ++t) {
        for (std::size_t i = 0; i < kPerThread; ++i) {
            EXPECT_EQ(reader.read(indexes[t][i]), test::makePayload(t + 1, i, 96));
        }
    }
}

TEST_F(ConcurrencyTest, BatchesFromManyThreadsStayContiguous) {
    constexpr std::uint32_t kThreads = 4;
    constexpr std::size_t kBatch = 20;

    RecordWriter writer(tmp.path(), fast);
    std::latch start(1);
    std::vector<std::thread> threads;
    std::vector<std::vector<INDEX_TYPE>> indexes(kThreads);
    for (std::uint32_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::vector<std::string> batch;
            for (std::size_t i = 0; i < kBatch; ++i) {
                batch.push_back(test::makePayload(t + 1, i, 64));
            }
            start.wait();
            indexes[t] = writer.appendBatch(batch);
        });
    }
    start.count_down();
    for (std::thread& thread : threads) {
        thread.join();
    }
    writer.close();

    // A batch is one unit: nobody else's record lands in the middle of it.
    for (std::uint32_t t = 0; t < kThreads; ++t) {
        ASSERT_EQ(indexes[t].size(), kBatch);
        for (std::size_t i = 1; i < kBatch; ++i) {
            EXPECT_EQ(offsetOf(indexes[t][i]) - offsetOf(indexes[t][i - 1]), format::recordSpan(64))
                << "thread " << t << " record " << i;
        }
    }
    expectExactlyOnce(kThreads, kBatch);
}

TEST_F(ConcurrencyTest, RandomReadsFromManyThreadsShareOneReader) {
    constexpr std::size_t kRecords = 400;
    std::vector<INDEX_TYPE> indexes;
    {
        RecordWriter writer(tmp.path(), fast);
        for (std::size_t i = 0; i < kRecords; ++i) {
            indexes.push_back(writer.append(test::makePayload(1, i, 200)));
        }
    }

    RecordReader reader(tmp.path());
    constexpr int kThreads = 8;
    std::latch start(1);
    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            start.wait();
            // Each thread walks the same records from a different starting
            // point, so the reads overlap without any randomness.
            for (std::size_t step = 0; step < kRecords; ++step) {
                const std::size_t i = (step + t * 37) % kRecords;
                if (reader.read(indexes[i]) != test::makePayload(1, i, 200)) {
                    ++mismatches;
                }
            }
        });
    }
    start.count_down();
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(mismatches.load(), 0);
}

// A shared reader hands each record to exactly one caller: iteration state is
// the reader's, not the thread's.
TEST_F(ConcurrencyTest, ReadNextFromManyThreadsHandsOutEachRecordOnce) {
    constexpr std::size_t kRecords = 500;
    {
        RecordWriter writer(tmp.path(), fast);
        for (std::size_t i = 0; i < kRecords; ++i) {
            writer.append(test::makePayload(1, i, 150));
        }
    }

    RecordReader reader(tmp.path());
    constexpr int kThreads = 6;
    std::latch start(1);
    std::vector<std::vector<std::string>> collected(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            start.wait();
            while (std::optional<std::string> value = reader.readNext()) {
                collected[t].push_back(std::move(*value));
            }
        });
    }
    start.count_down();
    for (std::thread& thread : threads) {
        thread.join();
    }

    std::set<std::uint64_t> seen;
    for (const std::vector<std::string>& per_thread : collected) {
        for (const std::string& value : per_thread) {
            ASSERT_TRUE(test::payloadIsIntact(value));
            EXPECT_TRUE(seen.insert(test::identifyPayload(value)->sequence).second)
                << "a record was handed out twice";
        }
    }
    EXPECT_EQ(seen.size(), kRecords);
}

// Writes become visible to a reader that is already open. The latches make the
// hand-off exact: the reader only looks after the round it is checking has
// been acknowledged, and the writer only continues once the reader has looked.
TEST_F(ConcurrencyTest, AReaderSeesEachRoundOfWritesAsItLands) {
    constexpr int kRounds = 6;
    constexpr std::size_t kPerRound = 25;

    std::vector<std::unique_ptr<std::latch>> written;
    std::vector<std::unique_ptr<std::latch>> checked;
    for (int r = 0; r < kRounds; ++r) {
        written.push_back(std::make_unique<std::latch>(1));
        checked.push_back(std::make_unique<std::latch>(1));
    }

    RecordWriter writer(tmp.path(), fast);
    std::thread producer([&] {
        for (int r = 0; r < kRounds; ++r) {
            for (std::size_t i = 0; i < kPerRound; ++i) {
                writer.append(test::makePayload(1, r * kPerRound + i, 120));
            }
            written[r]->count_down();
            checked[r]->wait();
        }
    });

    RecordReader reader(tmp.path());
    std::vector<std::string> seen;
    for (int r = 0; r < kRounds; ++r) {
        written[r]->wait();
        while (std::optional<std::string> value = reader.readNext()) {
            seen.push_back(std::move(*value));
        }
        EXPECT_EQ(seen.size(), (r + 1) * kPerRound) << "after round " << r;
        checked[r]->count_down();
    }
    producer.join();

    for (std::size_t i = 0; i < seen.size(); ++i) {
        EXPECT_EQ(seen[i], test::makePayload(1, i, 120)) << "record " << i;
    }
}

// Readers running alongside writers never see a record that is only partly
// written -- the checksums are what rule that out.
TEST_F(ConcurrencyTest, ReadersAndWritersRunTogether) {
    constexpr std::uint32_t kWriters = 4;
    constexpr std::size_t kPerWriter = 250;
    constexpr int kReaders = 3;

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
    for (int r = 0; r < kReaders; ++r) {
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
    expectExactlyOnce(kWriters, kPerWriter);
}

// Opening and closing writers from several threads at once, over and over: no
// two ever end up on the same segment, and nothing written gets lost.
TEST_F(ConcurrencyTest, WritersOpenedAndClosedRepeatedly) {
    constexpr std::uint32_t kThreads = 6;
    constexpr int kRounds = 15;

    std::latch start(1);
    std::vector<std::thread> threads;
    for (std::uint32_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            start.wait();
            for (int r = 0; r < kRounds; ++r) {
                RecordWriter writer(tmp.path(), fast);
                writer.append(test::makePayload(t + 1, r, 64));
                writer.close();
            }
        });
    }
    start.count_down();
    for (std::thread& thread : threads) {
        thread.join();
    }

    expectExactlyOnce(kThreads, kRounds);
    EXPECT_LE(test::segmentPaths(tmp.path()).size(), kThreads)
        << "closed segments must be reused, not abandoned";
}

TEST_F(ConcurrencyTest, DurableAppendsFromManyThreads) {
    // The same shape as the first test but with the durability the API
    // actually promises, at a size that keeps the sync writes reasonable.
    constexpr std::uint32_t kThreads = 4;
    constexpr std::size_t kPerThread = 25;

    std::latch start(1);
    std::vector<std::thread> threads;
    for (std::uint32_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            RecordWriter writer(tmp.path());
            start.wait();
            for (std::size_t i = 0; i < kPerThread; ++i) {
                writer.append(test::makePayload(t + 1, i, 4096));
            }
        });
    }
    start.count_down();
    for (std::thread& thread : threads) {
        thread.join();
    }
    expectExactlyOnce(kThreads, kPerThread);
}

}  // namespace
}  // namespace raf
