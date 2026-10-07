// Crash recovery.
//
// A process that dies is simulated by forking and leaving the child through
// _exit or SIGKILL, so no destructor runs and nothing is closed. The contract
// being checked each time: every record whose append returned is still there
// afterwards, no partial record is ever mistaken for a complete one, and the
// next writer picks up exactly where the last one stopped.
#include <gtest/gtest.h>

#include <signal.h>

#include <string>
#include <vector>

#include <raf/raf.h>

#include "test_support.h"

namespace raf {
namespace {

constexpr std::size_t kRecordSize = 256;
constexpr std::uint64_t kSpan = format::kMetaSize + kRecordSize;

class RecoveryTest : public ::testing::Test {
protected:
    test::TempArchive tmp{"raf-recovery"};

    // Appends `count` records in a child process that never closes anything.
    // Every append returned before the child died, so every one of them is
    // covered by the durability promise.
    int crashAfterAppending(std::uint32_t writer_id, std::size_t count, std::size_t first = 0,
                            bool by_signal = false) {
        return test::runInChild([&] {
            RecordWriter writer(tmp.path());
            for (std::size_t i = 0; i < count; ++i) {
                writer.append(test::makePayload(writer_id, first + i, kRecordSize));
            }
            if (by_signal) {
                ::raise(SIGKILL);
            }
            return 0;
        });
    }

    static std::vector<std::string> expected(std::uint32_t writer_id, std::size_t count,
                                             std::size_t first = 0) {
        std::vector<std::string> values;
        for (std::size_t i = 0; i < count; ++i) {
            values.push_back(test::makePayload(writer_id, first + i, kRecordSize));
        }
        return values;
    }
};

TEST_F(RecoveryTest, RecordsSurviveAWriterThatNeverClosed) {
    ASSERT_TRUE(test::exitedWith(crashAfterAppending(0, 16), 0));
    EXPECT_EQ(test::drain(tmp.path()), expected(0, 16));
}

TEST_F(RecoveryTest, RecordsSurviveSigkill) {
    ASSERT_TRUE(test::killedBy(crashAfterAppending(0, 16, 0, /*by_signal=*/true), SIGKILL));
    EXPECT_EQ(test::drain(tmp.path()), expected(0, 16));
}

TEST_F(RecoveryTest, IndexesHandedOutBeforeACrashStillResolve) {
    const std::string index_file = tmp.root() + "/indexes";
    ASSERT_TRUE(test::exitedWith(
        test::runInChild([&] {
            RecordWriter writer(tmp.path());
            std::vector<INDEX_TYPE> indexes;
            for (std::size_t i = 0; i < 12; ++i) {
                indexes.push_back(writer.append(test::makePayload(0, i, kRecordSize)));
            }
            const int fd = ::open(index_file.c_str(), O_CREAT | O_WRONLY, 0644);
            if (fd < 0) {
                return 1;
            }
            const ssize_t n = ::write(fd, indexes.data(), indexes.size() * sizeof(INDEX_TYPE));
            ::fsync(fd);
            ::close(fd);
            return n > 0 ? 0 : 1;
        }),
        0));

    const std::string raw = test::readRaw(index_file, 0, 12 * sizeof(INDEX_TYPE));
    ASSERT_EQ(raw.size(), 12 * sizeof(INDEX_TYPE));
    std::vector<INDEX_TYPE> indexes(12);
    std::memcpy(indexes.data(), raw.data(), raw.size());

    RecordReader reader(tmp.path());
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        EXPECT_EQ(reader.read(indexes[i]), test::makePayload(0, i, kRecordSize)) << "record " << i;
    }
}

TEST_F(RecoveryTest, TheNextWriterTakesOverTheCrashedWritersSegment) {
    ASSERT_TRUE(test::killedBy(crashAfterAppending(0, 4, 0, true), SIGKILL));
    ASSERT_EQ(test::segmentPaths(tmp.path()).size(), 1u);

    RecordWriter writer(tmp.path());
    EXPECT_EQ(writer.segmentId(), 0u) << "a dead process holds no lock";
    EXPECT_EQ(test::segmentPaths(tmp.path()).size(), 1u);
}

TEST_F(RecoveryTest, RecordsWrittenAfterRecoveryDoNotOverwriteTheOldOnes) {
    ASSERT_TRUE(test::killedBy(crashAfterAppending(0, 10, 0, true), SIGKILL));

    {
        RecordWriter writer(tmp.path());
        for (std::size_t i = 0; i < 5; ++i) {
            writer.append(test::makePayload(0, 10 + i, kRecordSize));
        }
        writer.close();
    }
    EXPECT_EQ(test::drain(tmp.path()), expected(0, 15));
    EXPECT_EQ(test::fileSize(test::segmentPaths(tmp.path()).front()),
              format::kHeaderSize + 15 * kSpan);
}

TEST_F(RecoveryTest, CrashingAndRecoveringRepeatedlyKeepsEverything) {
    constexpr std::size_t kRounds = 5;
    constexpr std::size_t kPerRound = 4;
    for (std::size_t round = 0; round < kRounds; ++round) {
        ASSERT_TRUE(test::killedBy(
            crashAfterAppending(0, kPerRound, round * kPerRound, /*by_signal=*/true), SIGKILL))
            << "round " << round;

        const std::vector<std::string> values = test::drain(tmp.path());
        EXPECT_EQ(values, expected(0, (round + 1) * kPerRound)) << "after round " << round;
        EXPECT_EQ(test::segmentPaths(tmp.path()).size(), 1u)
            << "recovery should reuse the segment, not leave a trail of them";
    }
}

// A crash lands anywhere inside a write, so the surviving file can end at any
// byte. Wherever it ends, what comes back has to be a prefix of what was
// written, with nothing half-read passed off as a record.
TEST_F(RecoveryTest, AFileCutAtAnyPointRecoversToARecordBoundary) {
    {
        RecordWriter writer(tmp.path());
        for (std::size_t i = 0; i < 6; ++i) {
            writer.append(test::makePayload(0, i, kRecordSize));
        }
        writer.close();
    }
    const std::string path = test::segmentPaths(tmp.path()).front();
    const std::string original = test::readRaw(path, 0, format::kHeaderSize + 6 * kSpan);
    ASSERT_EQ(original.size(), format::kHeaderSize + 6 * kSpan);

    // Cut points chosen to land on every interesting spot: exactly on a record
    // boundary, one byte in, inside a header, inside a payload, one byte short.
    const std::vector<std::uint64_t> offsets = {
        format::kHeaderSize,
        format::kHeaderSize + 1,
        format::kHeaderSize + 31,
        format::kHeaderSize + format::kMetaSize,
        format::kHeaderSize + kSpan / 2,
        format::kHeaderSize + kSpan - 1,
        format::kHeaderSize + kSpan,
        format::kHeaderSize + kSpan + 7,
        format::kHeaderSize + 3 * kSpan,
        format::kHeaderSize + 5 * kSpan + format::kMetaSize + 1,
        format::kHeaderSize + 6 * kSpan};

    for (const std::uint64_t cut : offsets) {
        test::writeRaw(path, 0, original.data(), original.size());
        test::truncateFile(path, cut);

        const std::size_t complete = static_cast<std::size_t>((cut - format::kHeaderSize) / kSpan);
        EXPECT_EQ(test::drain(tmp.path()), expected(0, complete)) << "cut at " << cut;

        // Reopening a writer trims the file back to that same boundary and
        // appends from there, so the next record cannot land in a hole.
        {
            RecordWriter writer(tmp.path());
            writer.append(test::makePayload(9, 0, kRecordSize));
            writer.close();
        }
        EXPECT_EQ(test::fileSize(path), format::kHeaderSize + (complete + 1) * kSpan)
            << "cut at " << cut;
        std::vector<std::string> after = expected(0, complete);
        after.push_back(test::makePayload(9, 0, kRecordSize));
        EXPECT_EQ(test::drain(tmp.path()), after) << "cut at " << cut;
    }
}

// Several writers dying at once is the same problem once per segment, since no
// writer shares a segment with another.
TEST_F(RecoveryTest, ConcurrentWritersAllRecover) {
    constexpr std::uint32_t kWriters = 4;
    constexpr std::size_t kPerWriter = 8;

    std::vector<int> statuses;
    for (std::uint32_t w = 0; w < kWriters; ++w) {
        // Each child holds its segment until it dies, so they land on
        // different segments.
        statuses.push_back(test::runInChild([&] {
            RecordWriter writer(tmp.path());
            for (std::size_t i = 0; i < kPerWriter; ++i) {
                writer.append(test::makePayload(w + 1, i, kRecordSize));
            }
            ::raise(SIGKILL);
            return 0;
        }));
    }
    for (const int status : statuses) {
        ASSERT_TRUE(test::killedBy(status, SIGKILL));
    }

    const std::vector<std::string> values = test::drain(tmp.path());
    EXPECT_EQ(values.size(), kWriters * kPerWriter);
    std::vector<std::vector<std::size_t>> seen(kWriters + 1);
    for (const std::string& value : values) {
        ASSERT_TRUE(test::payloadIsIntact(value));
        const test::PayloadId id = *test::identifyPayload(value);
        ASSERT_GE(id.writer, 1u);
        ASSERT_LE(id.writer, kWriters);
        seen[id.writer].push_back(id.sequence);
    }
    for (std::uint32_t w = 1; w <= kWriters; ++w) {
        std::vector<std::size_t> want(kPerWriter);
        for (std::size_t i = 0; i < kPerWriter; ++i) {
            want[i] = i;
        }
        EXPECT_EQ(seen[w], want) << "writer " << w << " lost or reordered records";
    }
}

TEST_F(RecoveryTest, RecoveryDoesNotDisturbAnUndamagedArchive) {
    {
        RecordWriter writer(tmp.path());
        for (std::size_t i = 0; i < 8; ++i) {
            writer.append(test::makePayload(0, i, kRecordSize));
        }
        writer.close();
    }
    const std::string path = test::segmentPaths(tmp.path()).front();
    const std::uint64_t size = test::fileSize(path);
    const std::string before = test::readRaw(path, 0, size);

    for (int i = 0; i < 3; ++i) {
        RecordWriter writer(tmp.path());
        writer.close();
    }
    EXPECT_EQ(test::fileSize(path), size);
    EXPECT_EQ(test::readRaw(path, 0, size), before)
        << "opening for recovery must not rewrite bytes";
}

}  // namespace
}  // namespace raf
