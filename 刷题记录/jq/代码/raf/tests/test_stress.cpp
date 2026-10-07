// Scale.
//
// Same correctness checks as the concurrency tests, run wide enough to shake
// out anything that only shows up under load: segment churn, many readers
// against many writers, records at the sizes the design targets. Set
// RAF_STRESS_SCALE to multiply the sizes for a longer soak.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <latch>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <raf/raf.h>

#include "store/format.h"
#include "test_support.h"

namespace raf {
namespace {

std::size_t scale() {
    const char* value = std::getenv("RAF_STRESS_SCALE");
    if (value == nullptr) {
        return 1;
    }
    const long parsed = std::strtol(value, nullptr, 10);
    return parsed > 0 ? static_cast<std::size_t>(parsed) : 1;
}

class StressTest : public ::testing::Test {
protected:
    test::TempArchive tmp{"raf-stress"};
    Options fast;

    StressTest() { fast.sync = false; }

    static void expectEachWriterInOrder(const std::vector<std::string>& values,
                                        std::uint32_t writers, std::size_t per_writer) {
        std::map<std::uint32_t, std::vector<std::uint64_t>> seen;
        for (const std::string& value : values) {
            ASSERT_TRUE(test::payloadIsIntact(value));
            const test::PayloadId id = *test::identifyPayload(value);
            seen[id.writer].push_back(id.sequence);
        }
        EXPECT_EQ(seen.size(), writers);
        for (std::uint32_t w = 1; w <= writers; ++w) {
            ASSERT_EQ(seen[w].size(), per_writer) << "writer " << w;
            for (std::size_t i = 0; i < per_writer; ++i) {
                EXPECT_EQ(seen[w][i], i) << "writer " << w << " position " << i;
            }
        }
    }
};

TEST_F(StressTest, ManyWritersAndReadersAtOnce) {
    const std::uint32_t writers = 16;
    const std::size_t per_writer = 800 * scale();
    constexpr int kReaders = 8;
    constexpr std::size_t kSize = 4096;

    std::latch start(1);
    // Every writer opens before any of them writes, so all of them really do
    // hold a segment at the same time.
    std::latch opened(writers);
    std::atomic<bool> writing{true};
    std::atomic<int> broken{0};
    std::atomic<std::size_t> read_count{0};

    std::vector<std::thread> writer_threads;
    for (std::uint32_t w = 0; w < writers; ++w) {
        writer_threads.emplace_back([&, w] {
            RecordWriter writer(tmp.path(), fast);
            opened.count_down();
            start.wait();
            for (std::size_t i = 0; i < per_writer; ++i) {
                writer.append(test::makePayload(w + 1, i, kSize));
            }
            writer.close();
        });
    }
    std::vector<std::thread> reader_threads;
    for (int r = 0; r < kReaders; ++r) {
        reader_threads.emplace_back([&] {
            start.wait();
            RecordReader reader(tmp.path());
            std::size_t count = 0;
            while (true) {
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
            read_count += count;
        });
    }

    opened.wait();
    const auto began = std::chrono::steady_clock::now();
    start.count_down();
    for (std::thread& thread : writer_threads) {
        thread.join();
    }
    writing.store(false);
    for (std::thread& thread : reader_threads) {
        thread.join();
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

    EXPECT_EQ(broken.load(), 0);
    EXPECT_GT(read_count.load(), 0u);
    expectEachWriterInOrder(test::drain(tmp.path()), writers, per_writer);
    EXPECT_EQ(test::segmentPaths(tmp.path()).size(), writers)
        << "writers that overlap in time never share a segment";

    const double bytes = static_cast<double>(writers * per_writer * kSize);
    std::fprintf(stderr, "[          ] %u writers x %zu x 4 KiB in %.2fs (%.0f MiB/s written)\n",
                 writers, per_writer, seconds, bytes / seconds / (1 << 20));
}

TEST_F(StressTest, LargeRecordsUnderLoad) {
    const std::uint32_t writers = 4;
    const std::size_t per_writer = 20 * scale();
    constexpr std::size_t kSize = 1u << 20;

    std::latch start(1);
    std::vector<std::thread> threads;
    for (std::uint32_t w = 0; w < writers; ++w) {
        threads.emplace_back([&, w] {
            RecordWriter writer(tmp.path(), fast);
            start.wait();
            for (std::size_t i = 0; i < per_writer; ++i) {
                writer.append(test::makePayload(w + 1, i, kSize));
            }
            writer.close();
        });
    }
    start.count_down();
    for (std::thread& thread : threads) {
        thread.join();
    }
    expectEachWriterInOrder(test::drain(tmp.path()), writers, per_writer);
}

// Small segments turn every few appends into a rollover, so the segment
// directory is constantly being re-read and re-claimed while writers run.
TEST_F(StressTest, SegmentChurn) {
    Options small = fast;
    small.maxSegmentBytes = format::kHeaderSize + 4096;
    const std::uint32_t writers = 8;
    const std::size_t per_writer = 100 * scale();

    std::latch start(1);
    std::vector<std::thread> threads;
    for (std::uint32_t w = 0; w < writers; ++w) {
        threads.emplace_back([&, w] {
            RecordWriter writer(tmp.path(), small);
            start.wait();
            for (std::size_t i = 0; i < per_writer; ++i) {
                writer.append(test::makePayload(w + 1, i, 512));
            }
            writer.close();
        });
    }
    start.count_down();
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_GT(test::segmentPaths(tmp.path()).size(), writers);
    expectEachWriterInOrder(test::drain(tmp.path()), writers, per_writer);
}

// Opening an archive walks and recovers every segment, so an archive with a
// lot of them has to stay quick and has to stay correct.
TEST_F(StressTest, ReopeningAnArchiveManyTimes) {
    const int rounds = 200 * static_cast<int>(scale());
    for (int r = 0; r < rounds; ++r) {
        RecordWriter writer(tmp.path(), fast);
        writer.append(test::makePayload(1, r, 256));
        writer.close();
    }

    const auto began = std::chrono::steady_clock::now();
    const std::vector<std::string> values = test::drain(tmp.path());
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

    ASSERT_EQ(values.size(), static_cast<std::size_t>(rounds));
    for (int r = 0; r < rounds; ++r) {
        EXPECT_EQ(values[r], test::makePayload(1, r, 256)) << "record " << r;
    }
    std::fprintf(stderr, "[          ] read %d records after %d reopens in %.3fs\n", rounds, rounds,
                 seconds);
}

TEST_F(StressTest, RandomReadsAcrossManySegments) {
    const std::uint32_t writers = 8;
    const std::size_t per_writer = 500 * scale();

    std::vector<std::vector<INDEX_TYPE>> indexes(writers);
    std::latch start(1);
    std::vector<std::thread> threads;
    for (std::uint32_t w = 0; w < writers; ++w) {
        threads.emplace_back([&, w] {
            RecordWriter writer(tmp.path(), fast);
            start.wait();
            for (std::size_t i = 0; i < per_writer; ++i) {
                indexes[w].push_back(writer.append(test::makePayload(w + 1, i, 1024)));
            }
            writer.close();
        });
    }
    start.count_down();
    for (std::thread& thread : threads) {
        thread.join();
    }

    RecordReader reader(tmp.path());
    constexpr int kReaderThreads = 8;
    std::latch reads(1);
    std::atomic<int> mismatches{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < kReaderThreads; ++t) {
        readers.emplace_back([&, t] {
            reads.wait();
            for (std::size_t step = 0; step < per_writer; ++step) {
                // A fixed stride per thread: every thread covers every record,
                // starting somewhere else, with nothing random about it.
                const std::size_t i = (step * 7 + t * 13) % per_writer;
                for (std::uint32_t w = 0; w < writers; ++w) {
                    if (reader.read(indexes[w][i]) != test::makePayload(w + 1, i, 1024)) {
                        ++mismatches;
                    }
                }
            }
        });
    }
    const auto began = std::chrono::steady_clock::now();
    reads.count_down();
    for (std::thread& thread : readers) {
        thread.join();
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

    EXPECT_EQ(mismatches.load(), 0);
    const double count = static_cast<double>(kReaderThreads * writers * per_writer);
    std::fprintf(stderr, "[          ] %.0f random reads in %.2fs (%.0f/s)\n", count, seconds,
                 count / seconds);
}

}  // namespace
}  // namespace raf
