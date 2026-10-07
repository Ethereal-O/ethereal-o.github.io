#include "io/io_backend.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace raf::io {
namespace {

// pwritev/preadv take at most IOV_MAX entries, and the kernel caps a single
// transfer just under 2 GiB. Both limits are handled by splitting, never by
// copying the caller's buffers together.
constexpr int kMaxIov = IOV_MAX;
constexpr std::size_t kMaxTransfer = 0x7ffff000u;

[[noreturn]] void throwErrno(int err, const std::string& what) {
    throw std::system_error(err, std::system_category(), what);
}

class PosixBackend final : public IoBackend {
public:
    int open(const std::string& path, const OpenFlags& flags) override {
        const int fd = ::open(path.c_str(), toOpenFlags(flags), 0644);
        if (fd < 0) {
            throwErrno(errno, "open " + path);
        }
        return fd;
    }

    int tryOpen(const std::string& path, const OpenFlags& flags, int* errno_out) override {
        const int fd = ::open(path.c_str(), toOpenFlags(flags), 0644);
        *errno_out = fd < 0 ? errno : 0;
        return fd;
    }

    void close(int fd) noexcept override {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    void writeAll(int fd, iovec* iov, int count, std::uint64_t offset) override {
        int first = 0;
        while (first < count) {
            const int batch = std::min(count - first, kMaxIov);
            std::size_t remaining = 0;
            int last = first;
            // Stop the batch before the transfer cap. A single iovec longer
            // than the cap is left alone; the short-write retry below splits
            // it instead.
            while (last < first + batch) {
                if (remaining != 0 && remaining + iov[last].iov_len > kMaxTransfer) {
                    break;
                }
                remaining += iov[last].iov_len;
                ++last;
            }
            while (remaining > 0) {
                const ssize_t n =
                    ::pwritev(fd, iov + first, last - first, static_cast<off_t>(offset));
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    throwErrno(errno, "pwritev");
                }
                if (n == 0) {
                    throwErrno(ENOSPC, "pwritev made no progress");
                }
                offset += static_cast<std::uint64_t>(n);
                remaining -= static_cast<std::size_t>(n);
                first = advance(iov, first, last, static_cast<std::size_t>(n));
            }
            first = last;
        }
    }

    std::size_t readSome(int fd, iovec* iov, int count, std::uint64_t offset) override {
        std::size_t total = 0;
        int first = 0;
        while (first < count) {
            const int last = std::min(count, first + kMaxIov);
            std::size_t want = 0;
            for (int i = first; i < last; ++i) {
                want += iov[i].iov_len;
            }
            ssize_t n = 0;
            while ((n = ::preadv(fd, iov + first, last - first, static_cast<off_t>(offset))) < 0) {
                if (errno != EINTR) {
                    throwErrno(errno, "preadv");
                }
            }
            total += static_cast<std::size_t>(n);
            // A short read is the end of the file, which is this call's whole
            // way of saying so -- the layers above use it to tell a record
            // still being written from a damaged one. It must not be retried.
            // On an O_DIRECT descriptor the retry was worse than redundant: a
            // file whose length is not a multiple of a sector returns a count
            // that is not either, so the second call asked for an unaligned
            // offset and a buffer that had been advanced to an unaligned
            // address, and the kernel answered EINVAL. Nothing reached that
            // path while a reader could only tail a file through the page
            // cache.
            if (static_cast<std::size_t>(n) < want) {
                return total;
            }
            offset += static_cast<std::uint64_t>(n);
            first = last;
        }
        return total;
    }

    void fsync(int fd) override {
        while (::fdatasync(fd) != 0) {
            if (errno != EINTR) {
                throwErrno(errno, "fdatasync");
            }
        }
    }

    void truncate(int fd, std::uint64_t length) override {
        while (::ftruncate(fd, static_cast<off_t>(length)) != 0) {
            if (errno != EINTR) {
                throwErrno(errno, "ftruncate");
            }
        }
    }

    std::uint64_t size(int fd) override {
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            throwErrno(errno, "fstat");
        }
        return static_cast<std::uint64_t>(st.st_size);
    }

    bool tryLockExclusive(int fd) override {
        while (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK) {
                return false;
            }
            if (errno != EINTR) {
                throwErrno(errno, "flock");
            }
        }
        return true;
    }

    void unlock(int fd) noexcept override { ::flock(fd, LOCK_UN); }

    void syncDirectory(const std::string& path) override {
        const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0) {
            throwErrno(errno, "open directory " + path);
        }
        const int rc = ::fsync(fd);
        const int err = errno;
        ::close(fd);
        if (rc != 0) {
            throwErrno(err, "fsync directory " + path);
        }
    }

    void makeDirectory(const std::string& path) override {
        if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
            throwErrno(errno, "mkdir " + path);
        }
    }

    bool directoryExists(const std::string& path) override {
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0) {
            return false;
        }
        return S_ISDIR(st.st_mode);
    }

    std::vector<std::string> listDirectory(const std::string& path) override {
        DIR* dir = ::opendir(path.c_str());
        if (dir == nullptr) {
            throwErrno(errno, "opendir " + path);
        }
        std::vector<std::string> names;
        errno = 0;
        while (const dirent* entry = ::readdir(dir)) {
            if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
                names.emplace_back(entry->d_name);
            }
            errno = 0;
        }
        const int err = errno;
        ::closedir(dir);
        if (err != 0) {
            throwErrno(err, "readdir " + path);
        }
        return names;
    }

private:
    static int toOpenFlags(const OpenFlags& flags) {
        int result = flags.read_only ? O_RDONLY : O_RDWR;
        if (flags.create) {
            result |= O_CREAT;
        }
        if (flags.exclusive) {
            result |= O_EXCL;
        }
        if (flags.sync) {
            result |= O_DSYNC;
        }
        if (flags.direct) {
            result |= O_DIRECT;
        }
        return result;
    }

    // Consumes `n` transferred bytes from iov[first..last) and returns the
    // first entry that still has bytes outstanding.
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

IoBackend& posixBackend() {
    static PosixBackend backend;
    return backend;
}

IoBackend& defaultBackend() {
    static IoBackend& selected = [&]() -> IoBackend& {
        const char* name = std::getenv("RAF_BACKEND");
#ifdef RAF_HAS_URING
        if (name != nullptr && std::strcmp(name, "uring") == 0) {
            return uringBackend();
        }
#endif
        (void)name;
        return posixBackend();
    }();
    return selected;
}

}  // namespace raf::io
