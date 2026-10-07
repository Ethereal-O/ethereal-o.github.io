// Processes.
//
// A writer's claim on a segment is an flock, which conflicts between open file
// descriptions rather than between processes -- the same mechanism that keeps
// two threads apart keeps two processes apart, and it survives a process that
// dies without cleaning up.
//
// Parent and child meet at pipe-backed gates rather than sleeps, so the
// ordering of events in these tests is fixed.
#include <gtest/gtest.h>

#include <signal.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <raf/raf.h>

#include "test_support.h"

namespace raf {
namespace {

// Children run their own process; a failed check comes back as an exit code
// rather than a gtest failure.
constexpr int kChildOk = 0;
constexpr int kChildFailed = 1;

class MultiProcessTest : public ::testing::Test {
protected:
    test::TempArchive tmp{"raf-process"};

    void expectExactlyOnce(std::uint32_t writers, std::size_t per_writer, std::size_t size) {
        const std::vector<std::string> values = test::drain(tmp.path());
        EXPECT_EQ(values.size(), writers * per_writer);
        std::map<std::uint32_t, std::vector<std::uint64_t>> seen;
        for (const std::string& value : values) {
            ASSERT_TRUE(test::payloadIsIntact(value));
            const test::PayloadId id = *test::identifyPayload(value);
            EXPECT_EQ(id.size, size);
            seen[id.writer].push_back(id.sequence);
        }
        for (std::uint32_t w = 1; w <= writers; ++w) {
            std::vector<std::uint64_t> want(per_writer);
            for (std::size_t i = 0; i < per_writer; ++i) {
                want[i] = i;
            }
            EXPECT_EQ(seen[w], want) << "process " << w;
        }
    }
};

TEST_F(MultiProcessTest, ProcessesNeverShareASegment) {
    constexpr int kChildren = 4;
    // The archive has to exist before the children race for segments,
    // otherwise they race to create the directory as well.
    { RecordWriter warmup(tmp.path()); }

    std::vector<std::unique_ptr<test::ChildGate>> gates;
    std::vector<pid_t> children;
    for (int c = 0; c < kChildren; ++c) {
        gates.push_back(std::make_unique<test::ChildGate>());
    }

    for (int c = 0; c < kChildren; ++c) {
        const pid_t pid = ::fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            int code = kChildFailed;
            try {
                RecordWriter writer(tmp.path());
                // Hold the segment while the parent looks, then let go.
                gates[c]->arrive(static_cast<unsigned char>(writer.segmentId()));
                code = kChildOk;
            } catch (...) {
            }
            ::_exit(code);
        }
        children.push_back(pid);
    }

    std::set<unsigned> claimed;
    for (int c = 0; c < kChildren; ++c) {
        EXPECT_TRUE(claimed.insert(gates[c]->waitForChild()).second)
            << "two processes claimed the same segment";
    }
    EXPECT_EQ(claimed.size(), kChildren);
    // While they are all holding on, a writer here has to go somewhere else.
    {
        RecordWriter mine(tmp.path());
        EXPECT_EQ(claimed.count(mine.segmentId()), 0u);
    }
    for (int c = 0; c < kChildren; ++c) {
        gates[c]->release();
    }
    for (const pid_t pid : children) {
        int status = 0;
        ASSERT_GE(::waitpid(pid, &status, 0), 0);
        EXPECT_TRUE(test::exitedWith(status, kChildOk));
    }
}

TEST_F(MultiProcessTest, RecordsFromSeveralProcessesAllLand) {
    constexpr std::uint32_t kChildren = 4;
    constexpr std::size_t kPerChild = 40;
    constexpr std::size_t kSize = 512;

    std::vector<int> statuses;
    std::vector<pid_t> children;
    for (std::uint32_t c = 0; c < kChildren; ++c) {
        const pid_t pid = ::fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            int code = kChildFailed;
            try {
                RecordWriter writer(tmp.path());
                for (std::size_t i = 0; i < kPerChild; ++i) {
                    writer.append(test::makePayload(c + 1, i, kSize));
                }
                writer.close();
                code = kChildOk;
            } catch (...) {
            }
            ::_exit(code);
        }
        children.push_back(pid);
    }
    for (const pid_t pid : children) {
        int status = 0;
        ASSERT_GE(::waitpid(pid, &status, 0), 0);
        ASSERT_TRUE(test::exitedWith(status, kChildOk));
    }
    expectExactlyOnce(kChildren, kPerChild, kSize);
}

// A process that dies holding a segment leaves no lock behind: the kernel
// drops it when the file description goes, and the next writer takes over.
TEST_F(MultiProcessTest, ASegmentHeldByADeadProcessIsTakenOver) {
    test::ChildGate gate;
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        int code = kChildFailed;
        try {
            RecordWriter writer(tmp.path());
            for (int i = 0; i < 5; ++i) {
                writer.append(test::makePayload(1, i, 128));
            }
            gate.arrive(static_cast<unsigned char>(writer.segmentId()));
            ::raise(SIGKILL);
            code = kChildOk;
        } catch (...) {
        }
        ::_exit(code);
    }

    const unsigned held = gate.waitForChild();
    {
        // The segment really is taken while the child is alive.
        RecordWriter mine(tmp.path());
        EXPECT_NE(mine.segmentId(), held);
    }
    gate.release();

    int status = 0;
    ASSERT_GE(::waitpid(pid, &status, 0), 0);
    ASSERT_TRUE(test::killedBy(status, SIGKILL));

    RecordWriter successor(tmp.path());
    EXPECT_EQ(successor.segmentId(), held) << "the dead process's segment is free again";
    successor.append(test::makePayload(2, 0, 128));
    successor.close();

    const std::vector<std::string> values = test::drain(tmp.path());
    EXPECT_EQ(values.size(), 6u) << "taking over must not disturb what was already written";
    for (const std::string& value : values) {
        EXPECT_TRUE(test::payloadIsIntact(value));
    }
}

TEST_F(MultiProcessTest, AReaderSeesAnotherProcessesWrites) {
    { RecordWriter warmup(tmp.path()); }
    RecordReader reader(tmp.path());
    EXPECT_EQ(reader.readNext(), std::nullopt);

    test::ChildGate gate;
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        int code = kChildFailed;
        try {
            RecordWriter writer(tmp.path());
            for (int i = 0; i < 10; ++i) {
                writer.append(test::makePayload(1, i, 256));
            }
            writer.close();
            gate.arrive();
            code = kChildOk;
        } catch (...) {
        }
        ::_exit(code);
    }

    gate.waitForChild();  // every append has returned, so every record is durable
    const std::vector<std::string> values = test::drain(reader);
    EXPECT_EQ(values.size(), 10u);
    for (std::size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(values[i], test::makePayload(1, i, 256));
    }
    gate.release();

    int status = 0;
    ASSERT_GE(::waitpid(pid, &status, 0), 0);
    EXPECT_TRUE(test::exitedWith(status, kChildOk));
}

// Writers in different processes take turns, one round each, with the gates
// forcing the interleaving rather than leaving it to chance.
TEST_F(MultiProcessTest, InterleavedProcessesKeepTheirOwnOrder) {
    constexpr int kRounds = 5;
    { RecordWriter warmup(tmp.path()); }

    test::ChildGate gate;
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        int code = kChildFailed;
        try {
            RecordWriter writer(tmp.path());
            for (int r = 0; r < kRounds; ++r) {
                writer.append(test::makePayload(1, r, 200));
                gate.arrive();
            }
            writer.close();
            code = kChildOk;
        } catch (...) {
        }
        ::_exit(code);
    }

    RecordWriter mine(tmp.path());
    for (int r = 0; r < kRounds; ++r) {
        gate.waitForChild();  // the child's record for this round is durable
        mine.append(test::makePayload(2, r, 200));
        gate.release();
    }
    mine.close();

    int status = 0;
    ASSERT_GE(::waitpid(pid, &status, 0), 0);
    ASSERT_TRUE(test::exitedWith(status, kChildOk));

    const std::vector<std::string> values = test::drain(tmp.path());
    ASSERT_EQ(values.size(), 2u * kRounds);
    std::map<std::uint32_t, std::vector<std::uint64_t>> seen;
    for (const std::string& value : values) {
        ASSERT_TRUE(test::payloadIsIntact(value));
        const test::PayloadId id = *test::identifyPayload(value);
        seen[id.writer].push_back(id.sequence);
    }
    for (std::uint32_t w = 1; w <= 2; ++w) {
        EXPECT_EQ(seen[w], (std::vector<std::uint64_t>{0, 1, 2, 3, 4})) << "process " << w;
    }
}

}  // namespace
}  // namespace raf
