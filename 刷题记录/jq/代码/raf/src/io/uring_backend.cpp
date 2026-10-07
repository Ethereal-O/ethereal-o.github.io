// IO layer: the io_uring implementation the design doc planned for.
//
// Only the data path -- writeAll, readSome, fsync -- goes through the ring.
// open/close/fstat/flock and the directory calls stay on the POSIX backend:
// flock has no io_uring opcode at all, and the rest run once per segment
// rather than once per record, so routing them through a ring would add code
// without moving a benchmark.
//
// Rings are not safe to submit to concurrently, so each thread gets its own.
// Because raf's API semantics make a returning write a durable write, a
// backend call cannot return before its completion arrives: every operation
// here is submit-then-wait, depth one. That is the honest shape of io_uring
// behind a synchronous interface, and measuring it is the point.
#include <liburing.h>

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <system_error>

#include "io/io_backend.h"

namespace raf::io {
namespace {

constexpr unsigned kQueueDepth = 64;
constexpr int kMaxIov = IOV_MAX;
constexpr std::size_t kMaxTransfer = 0x7ffff000u;
// With SQPOLL the kernel picks submissions up on its own, so the only reason
// to enter the kernel is to wait. Spin on the completion queue briefly first:
// on a fast device the answer is often already there.
constexpr int kPollSpins = 512;

[[noreturn]] void throwErrno(int err, const std::string& what) {
    throw std::system_error(err, std::system_category(), what);
}

bool sqpollRequested() {
    const char* value = std::getenv("RAF_URING_SQPOLL");
    return value != nullptr && value[0] == '1';
}

// One SQPOLL kernel thread per ring would put a poller per application thread
// on the machine, which at high writer counts costs more CPU than it saves.
// IORING_SETUP_ATTACH_WQ points later rings at the first ring's poll thread so
// they all share one.
std::mutex& attachMutex() {
    static std::mutex m;
    return m;
}
int& attachFd() {
    static int fd = -1;
    return fd;
}

// A ring does not survive fork. The submission and completion queues are a
// shared mapping, so parent and child end up driving the same head/tail
// cursors: the child's submission consumes the parent's slot, and the parent's
// next io_uring_submit silently returns 0 and then blocks forever waiting for
// a completion that will never be posted. raf's contract allows multi-process
// writers, so this has to be handled rather than documented away.
//
// The fix is to notice the fork and build a fresh ring before the inherited
// one is ever used. Only the forking thread exists in the child, so only its
// ring is reachable there; rings belonging to the parent's other threads are
// left mapped in the child and go away when it exits or execs.
std::atomic<unsigned>& forkGeneration() {
    static std::atomic<unsigned> generation{0};
    return generation;
}

void registerForkHandlerOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        pthread_atfork(nullptr, nullptr,
                       [] { forkGeneration().fetch_add(1, std::memory_order_relaxed); });
    });
}

// A ring plus its lifetime. thread_local, so submission needs no lock.
class Ring {
public:
    Ring() {
        registerForkHandlerOnce();
        openRing();
    }

    ~Ring() { io_uring_queue_exit(&ring_); }

    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    io_uring* get() {
        const unsigned current = forkGeneration().load(std::memory_order_relaxed);
        if (current != generation_) [[unlikely]] {
            // Tear down the inherited mapping -- munmap and close here only
            // touch this process's copy, so the parent's ring is unaffected --
            // and open a ring that belongs to this process.
            io_uring_queue_exit(&ring_);
            ring_ = io_uring{};
            {
                // The parent's poll thread belongs to the parent; this process
                // starts its own.
                const std::lock_guard<std::mutex> guard(attachMutex());
                attachFd() = -1;
            }
            openRing();
            generation_ = current;
        }
        return &ring_;
    }

private:
    void openRing() {
        io_uring_params params{};
        if (!sqpollRequested()) {
            initOrThrow(params);
            return;
        }
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = 2000;
        const std::lock_guard<std::mutex> guard(attachMutex());
        if (attachFd() >= 0) {
            params.flags |= IORING_SETUP_ATTACH_WQ;
            params.wq_fd = static_cast<unsigned>(attachFd());
        }
        initOrThrow(params);
        if (attachFd() < 0) {
            attachFd() = ring_.ring_fd;
        }
    }

    void initOrThrow(io_uring_params& params) {
        const int rc = io_uring_queue_init_params(kQueueDepth, &ring_, &params);
        if (rc < 0) {
            throwErrno(-rc, "io_uring_queue_init");
        }
    }

    io_uring ring_{};
    unsigned generation_ = forkGeneration().load(std::memory_order_relaxed);
};

io_uring* ring() {
    static thread_local Ring instance;
    return instance.get();
}

io_uring_sqe* sqe(io_uring* r) {
    io_uring_sqe* s = io_uring_get_sqe(r);
    if (s == nullptr) {
        // Depth one means the queue cannot actually fill, but a missing sqe
        // would otherwise be a null dereference.
        throwErrno(EBUSY, "io_uring_get_sqe");
    }
    return s;
}

// Submits the one prepared sqe and returns its result, negative errno and all.
// EINTR is left to the caller so the retry loops read like the POSIX ones.
int submitAndWait(io_uring* r) {
    const int submitted = io_uring_submit(r);
    if (submitted < 0) {
        if (submitted == -EINTR) {
            return -EINTR;
        }
        throwErrno(-submitted, "io_uring_submit");
    }
    io_uring_cqe* cqe = nullptr;
    int rc = -EAGAIN;
    for (int spin = 0; spin < kPollSpins && rc == -EAGAIN; ++spin) {
        rc = io_uring_peek_cqe(r, &cqe);
    }
    if (rc == -EAGAIN) {
        rc = io_uring_wait_cqe(r, &cqe);
    }
    if (rc < 0) {
        if (rc == -EINTR) {
            return -EINTR;
        }
        throwErrno(-rc, "io_uring_wait_cqe");
    }
    const int result = cqe->res;
    io_uring_cqe_seen(r, cqe);
    return result;
}

class UringBackend final : public IoBackend {
public:
    // Metadata and locking stay on the POSIX path; see the file comment.
    int open(const std::string& path, const OpenFlags& flags) override {
        return posixBackend().open(path, flags);
    }
    int tryOpen(const std::string& path, const OpenFlags& flags, int* errno_out) override {
        return posixBackend().tryOpen(path, flags, errno_out);
    }
    void close(int fd) noexcept override { posixBackend().close(fd); }
    void truncate(int fd, std::uint64_t length) override { posixBackend().truncate(fd, length); }
    std::uint64_t size(int fd) override { return posixBackend().size(fd); }
    bool tryLockExclusive(int fd) override { return posixBackend().tryLockExclusive(fd); }
    void unlock(int fd) noexcept override { posixBackend().unlock(fd); }
    void syncDirectory(const std::string& path) override { posixBackend().syncDirectory(path); }
    void makeDirectory(const std::string& path) override { posixBackend().makeDirectory(path); }
    bool directoryExists(const std::string& path) override {
        return posixBackend().directoryExists(path);
    }
    std::vector<std::string> listDirectory(const std::string& path) override {
        return posixBackend().listDirectory(path);
    }

    // The splitting and short-transfer retries mirror the POSIX backend
    // exactly; only the call in the middle differs. The fd keeps its O_DSYNC
    // flag through the ring, so a completion still means a durable write.
    void writeAll(int fd, iovec* iov, int count, std::uint64_t offset) override {
        io_uring* r = ring();
        int first = 0;
        while (first < count) {
            const int batch = std::min(count - first, kMaxIov);
            std::size_t remaining = 0;
            int last = first;
            while (last < first + batch) {
                if (remaining != 0 && remaining + iov[last].iov_len > kMaxTransfer) {
                    break;
                }
                remaining += iov[last].iov_len;
                ++last;
            }
            while (remaining > 0) {
                io_uring_prep_writev(sqe(r), fd, iov + first, last - first, offset);
                const int n = submitAndWait(r);
                if (n < 0) {
                    if (n == -EINTR) {
                        continue;
                    }
                    throwErrno(-n, "io_uring writev");
                }
                if (n == 0) {
                    throwErrno(ENOSPC, "io_uring writev made no progress");
                }
                offset += static_cast<std::uint64_t>(n);
                remaining -= static_cast<std::size_t>(n);
                first = advance(iov, first, last, static_cast<std::size_t>(n));
            }
            first = last;
        }
    }

    std::size_t readSome(int fd, iovec* iov, int count, std::uint64_t offset) override {
        io_uring* r = ring();
        std::size_t total = 0;
        int first = 0;
        while (first < count) {
            const int last = std::min(count, first + kMaxIov);
            std::size_t want = 0;
            for (int i = first; i < last; ++i) {
                want += iov[i].iov_len;
            }
            while (want > 0) {
                io_uring_prep_readv(sqe(r), fd, iov + first, last - first, offset);
                const int n = submitAndWait(r);
                if (n < 0) {
                    if (n == -EINTR) {
                        continue;
                    }
                    throwErrno(-n, "io_uring readv");
                }
                if (n == 0) {
                    return total;  // end of file
                }
                total += static_cast<std::size_t>(n);
                offset += static_cast<std::uint64_t>(n);
                want -= static_cast<std::size_t>(n);
                first = advance(iov, first, last, static_cast<std::size_t>(n));
            }
            first = last;
        }
        return total;
    }

    void fsync(int fd) override {
        io_uring* r = ring();
        for (;;) {
            io_uring_prep_fsync(sqe(r), fd, IORING_FSYNC_DATASYNC);
            const int n = submitAndWait(r);
            if (n == 0) {
                return;
            }
            if (n != -EINTR) {
                throwErrno(-n, "io_uring fdatasync");
            }
        }
    }

private:
    static int advance(iovec* iov, int first, int last, std::size_t n) {
        while (first < last && n > 0) {
            const std::size_t take = std::min(n, iov[first].iov_len);
            iov[first].iov_base = static_cast<char*>(iov[first].iov_base) + take;
            iov[first].iov_len -= take;
            n -= take;
            if (iov[first].iov_len == 0) {
                ++first;
            }
        }
        return first;
    }
};

}  // namespace

IoBackend& uringBackend() {
    static UringBackend backend;
    return backend;
}

}  // namespace raf::io
