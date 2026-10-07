// The IO layer, below the format.
//
// Every backend raf ships has to pass the same tests: the layers above name
// no POSIX API, so a backend is interchangeable or it is nothing. The suite
// is parameterised over the ones built in -- POSIX always, io_uring when
// RAF_URING is on -- and skips a backend the running kernel cannot provide
// rather than failing on it.
#include <gtest/gtest.h>

#include <signal.h>
#include <sys/resource.h>
#include <sys/uio.h>

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

#include "io/io_backend.h"
#include "test_support.h"

namespace raf {
namespace {

using io::OpenFlags;

struct BackendUnderTest {
    const char* name;
    io::IoBackend& (*get)();
};

class BackendTest : public ::testing::TestWithParam<BackendUnderTest> {
protected:
    io::IoBackend* backend_ = nullptr;
    test::TempArchive tmp{"raf-backend"};

    // A backend can be compiled in and still be unavailable: io_uring is
    // restricted or absent on plenty of kernels. Find out with one real
    // write rather than assuming.
    void SetUp() override {
        backend_ = &GetParam().get();
        try {
            const int fd = create(file("probe"));
            char byte = 'x';
            iovec iov = {&byte, 1};
            backend_->writeAll(fd, &iov, 1, 0);
            backend_->close(fd);
        } catch (const std::system_error& e) {
            GTEST_SKIP() << GetParam().name << " is unavailable here: " << e.what();
        }
    }

    io::IoBackend& backend() { return *backend_; }

    std::string file(const char* name) const { return tmp.root() + "/" + name; }

    int create(const std::string& path, bool sync = false) {
        OpenFlags flags;
        flags.create = true;
        flags.sync = sync;
        return backend_->open(path, flags);
    }
};

TEST_P(BackendTest, WritesAndReadsBack) {
    const int fd = create(file("basic"));
    const std::string first = "hello";
    const std::string second = "world";
    iovec iov[2] = {{const_cast<char*>(first.data()), first.size()},
                    {const_cast<char*>(second.data()), second.size()}};
    backend().writeAll(fd, iov, 2, 0);
    EXPECT_EQ(backend().size(fd), first.size() + second.size());

    std::string buffer(16, '\0');
    iovec in = {buffer.data(), buffer.size()};
    EXPECT_EQ(backend().readSome(fd, &in, 1, 0), 10u);
    EXPECT_EQ(buffer.substr(0, 10), "helloworld");
    backend().close(fd);
}

TEST_P(BackendTest, ScattersAcrossMoreIovecsThanTheKernelTakesAtOnce) {
    const int fd = create(file("scatter"));
    // IOV_MAX is 1024 on Linux; going past it has to split rather than fail.
    constexpr int kCount = 3000;
    std::vector<std::string> pieces;
    pieces.reserve(kCount);
    std::vector<iovec> iov;
    iov.reserve(kCount);
    std::string expected;
    for (int i = 0; i < kCount; ++i) {
        pieces.push_back(std::to_string(i) + ";");
        expected += pieces.back();
    }
    for (std::string& piece : pieces) {
        iov.push_back({piece.data(), piece.size()});
    }
    backend().writeAll(fd, iov.data(), kCount, 0);
    EXPECT_EQ(backend().size(fd), expected.size());

    std::string buffer(expected.size(), '\0');
    iovec in = {buffer.data(), buffer.size()};
    EXPECT_EQ(backend().readSome(fd, &in, 1, 0), expected.size());
    EXPECT_EQ(buffer, expected);
    backend().close(fd);
}

// A short read is how the layers above tell "not written yet" from "damaged",
// so it has to come back as a count rather than an error.
TEST_P(BackendTest, ReadPastTheEndReturnsWhatIsThere) {
    const int fd = create(file("short"));
    const std::string data = "1234";
    iovec out = {const_cast<char*>(data.data()), data.size()};
    backend().writeAll(fd, &out, 1, 0);

    std::string a(3, '\0');
    std::string b(8, '\0');
    iovec in[2] = {{a.data(), a.size()}, {b.data(), b.size()}};
    EXPECT_EQ(backend().readSome(fd, in, 2, 0), 4u);
    EXPECT_EQ(a, "123");
    EXPECT_EQ(b[0], '4');

    iovec beyond = {b.data(), b.size()};
    EXPECT_EQ(backend().readSome(fd, &beyond, 1, 100), 0u);
    backend().close(fd);
}

TEST_P(BackendTest, TruncateAndSizeAgree) {
    const int fd = create(file("truncate"));
    const std::string data(1000, 'z');
    iovec out = {const_cast<char*>(data.data()), data.size()};
    backend().writeAll(fd, &out, 1, 0);
    EXPECT_EQ(backend().size(fd), 1000u);
    backend().truncate(fd, 100);
    EXPECT_EQ(backend().size(fd), 100u);
    backend().truncate(fd, 0);
    EXPECT_EQ(backend().size(fd), 0u);
    backend().close(fd);
}

// One lock serves both the multi-thread and the multi-process case because
// flock conflicts between file descriptions, not between processes.
TEST_P(BackendTest, ExclusiveLockConflictsWithinOneProcess) {
    const std::string path = file("locked");
    const int first = create(path);
    const int second = create(path);

    EXPECT_TRUE(backend().tryLockExclusive(first));
    EXPECT_FALSE(backend().tryLockExclusive(second));

    backend().unlock(first);
    EXPECT_TRUE(backend().tryLockExclusive(second));
    backend().unlock(second);

    // Closing the holder releases the lock too.
    EXPECT_TRUE(backend().tryLockExclusive(first));
    backend().close(first);
    EXPECT_TRUE(backend().tryLockExclusive(second));
    backend().close(second);
}

TEST_P(BackendTest, OpenReportsFailures) {
    OpenFlags flags;
    EXPECT_THROW(backend().open(file("missing"), flags), std::system_error);

    int err = 0;
    EXPECT_LT(backend().tryOpen(file("missing"), flags, &err), 0);
    EXPECT_EQ(err, ENOENT);

    OpenFlags exclusive;
    exclusive.create = true;
    exclusive.exclusive = true;
    const int fd = backend().open(file("once"), exclusive);
    EXPECT_GE(fd, 0);
    backend().close(fd);
    EXPECT_LT(backend().tryOpen(file("once"), exclusive, &err), 0);
    EXPECT_EQ(err, EEXIST);
}

TEST_P(BackendTest, DirectoryOperations) {
    const std::string dir = file("sub");
    EXPECT_FALSE(backend().directoryExists(dir));
    backend().makeDirectory(dir);
    EXPECT_TRUE(backend().directoryExists(dir));
    backend().makeDirectory(dir);  // again: not an error

    backend().close(create(dir + "/a"));
    backend().close(create(dir + "/b"));
    std::vector<std::string> names = backend().listDirectory(dir);
    std::sort(names.begin(), names.end());
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "a");
    EXPECT_EQ(names[1], "b");

    backend().syncDirectory(dir);
    EXPECT_THROW(backend().listDirectory(file("nowhere")), std::system_error);
}

TEST_P(BackendTest, SyncWritesSurviveWithoutAnExplicitFlush) {
    const std::string path = file("dsync");
    const int fd = create(path, /*sync=*/true);
    const std::string data = "durable";
    iovec out = {const_cast<char*>(data.data()), data.size()};
    backend().writeAll(fd, &out, 1, 0);
    // O_DSYNC means the write call itself did the flushing; nothing else in
    // raf makes an append durable.
    EXPECT_EQ(test::readRaw(path, 0, data.size()), data);
    backend().close(fd);
}

// A write that cannot finish must come back as an error with the prefix it
// managed on disk -- never a silent short write, and never a retry loop that
// cannot make progress. RLIMIT_FSIZE produces a genuine short pwritev: the
// kernel writes up to the limit, returns the count, and fails the next
// attempt with EFBIG.
TEST_P(BackendTest, AWriteThatCannotFinishFailsWithItsPrefixOnDisk) {
    constexpr std::size_t kLimit = 8 * 1024;
    const std::string path = file("clipped");
    const int fd = create(path);

    rlimit original{};
    ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &original), 0);
    // Without this the kernel kills the process instead of failing the write.
    const auto previous_handler = ::signal(SIGXFSZ, SIG_IGN);
    rlimit limited = original;
    limited.rlim_cur = kLimit;
    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &limited), 0);

    const std::string data(4 * kLimit, 'w');
    iovec out = {const_cast<char*>(data.data()), data.size()};
    bool threw = false;
    try {
        backend().writeAll(fd, &out, 1, 0);
    } catch (const std::system_error&) {
        threw = true;
    }

    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &original), 0);
    ::signal(SIGXFSZ, previous_handler);

    EXPECT_TRUE(threw) << "a write that cannot complete must not return quietly";
    EXPECT_EQ(backend().size(fd), kLimit) << "the bytes that fit should have landed";
    backend().close(fd);
}

const BackendUnderTest kBackends[] = {
    {"posix", io::posixBackend},
#ifdef RAF_HAS_URING
    {"uring", io::uringBackend},
#endif
};

INSTANTIATE_TEST_SUITE_P(Backends, BackendTest, ::testing::ValuesIn(kBackends),
                         [](const ::testing::TestParamInfo<BackendUnderTest>& info) {
                             return std::string(info.param.name);
                         });

}  // namespace
}  // namespace raf
