// IO layer: every system call raf makes against a file goes through here,
// so the layers above never name a POSIX API. The design doc plans io_uring
// behind this same interface; the POSIX implementation is what ships today.
#pragma once

#include <sys/uio.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace raf::io {

struct OpenFlags {
    bool read_only = false;
    bool create = false;     // O_CREAT
    bool exclusive = false;  // O_EXCL, only meaningful with create
    bool sync = false;       // O_DSYNC: a returning write is a durable write
    // O_DIRECT: the drive transfers straight to and from the caller's buffer,
    // with no page cache in between. Every offset, every length and every
    // buffer address must then be a multiple of the device's logical block
    // size; store::ReadWindow is what satisfies that for the read path.
    bool direct = false;
};

class IoBackend {
public:
    virtual ~IoBackend() = default;

    // Returns an open descriptor, or throws std::system_error.
    virtual int open(const std::string& path, const OpenFlags& flags) = 0;
    // Same, but reports failure through `errno_out` and a negative return
    // instead of throwing, for the cases where a failure is expected -- a
    // segment that another writer created first, or one deleted since we
    // listed the directory.
    virtual int tryOpen(const std::string& path, const OpenFlags& flags, int* errno_out) = 0;
    virtual void close(int fd) noexcept = 0;

    // Writes every byte the iovecs describe. Short writes are retried, and
    // runs longer than IOV_MAX are split, so a return means all of it landed.
    // `iov` is modified in place while retrying -- passing a scratch array
    // rather than a copy keeps the payload buffers untouched.
    virtual void writeAll(int fd, iovec* iov, int count, std::uint64_t offset) = 0;

    // Reads up to what the iovecs describe. A short return means end of file,
    // not an error; callers use that to detect a record still being written.
    virtual std::size_t readSome(int fd, iovec* iov, int count, std::uint64_t offset) = 0;

    virtual void fsync(int fd) = 0;
    virtual void truncate(int fd, std::uint64_t length) = 0;
    virtual std::uint64_t size(int fd) = 0;

    // Advisory whole-file lock, flock(2) style: held by the open file
    // description, so two descriptors conflict even inside one process. That
    // is what makes one lock serve both the multi-thread and multi-process
    // cases. Returns false if someone else holds it.
    virtual bool tryLockExclusive(int fd) = 0;
    virtual void unlock(int fd) noexcept = 0;

    // Makes a directory's entries durable, so a freshly created segment file
    // survives a crash along with the records written into it.
    virtual void syncDirectory(const std::string& path) = 0;
    virtual void makeDirectory(const std::string& path) = 0;
    virtual bool directoryExists(const std::string& path) = 0;
    virtual std::vector<std::string> listDirectory(const std::string& path) = 0;
};

// The default backend: pread/pwrite against files opened with O_DSYNC.
IoBackend& posixBackend();

#ifdef RAF_HAS_URING
// io_uring for the data path, POSIX for metadata and locking. Built only when
// liburing is present; see src/io/uring_backend.cpp.
IoBackend& uringBackend();
#endif

// What the api layer opens with. Chosen once, from the RAF_BACKEND environment
// variable: "posix" (the default) or "uring". An unknown or unavailable name
// falls back to POSIX rather than failing, so a binary built without liburing
// still runs when the variable is set.
IoBackend& defaultBackend();

}  // namespace raf::io
