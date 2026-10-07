// Backends that forward to a real one and misbehave on request.
//
// Everything raf does to a file goes through IoBackend, so this is where a
// failure can be injected that the filesystem will not produce on demand: a
// write that lands halfway and then fails, an fsync that reports EIO. The
// design doc asks for both by name, and for the property they test -- that a
// failure never lets a partial record pass for a complete one, and never
// leaves the writer unable to continue.
//
// It is also the only way to assert on a call that leaves no trace in the
// result, which is what `syncDirectory` is: the durability of a new
// segment's directory entry cannot be observed by reading the archive back.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#include "io/io_backend.h"

namespace raf::test {

// Forwards every call unchanged. Subclasses override only what they bend.
class ForwardingBackend : public io::IoBackend {
public:
    explicit ForwardingBackend(io::IoBackend& inner) : inner_(inner) {}

    int open(const std::string& path, const io::OpenFlags& flags) override {
        return inner_.open(path, flags);
    }
    int tryOpen(const std::string& path, const io::OpenFlags& flags, int* errno_out) override {
        return inner_.tryOpen(path, flags, errno_out);
    }
    void close(int fd) noexcept override { inner_.close(fd); }
    void writeAll(int fd, iovec* iov, int count, std::uint64_t offset) override {
        inner_.writeAll(fd, iov, count, offset);
    }
    std::size_t readSome(int fd, iovec* iov, int count, std::uint64_t offset) override {
        return inner_.readSome(fd, iov, count, offset);
    }
    void fsync(int fd) override { inner_.fsync(fd); }
    void truncate(int fd, std::uint64_t length) override { inner_.truncate(fd, length); }
    std::uint64_t size(int fd) override { return inner_.size(fd); }
    bool tryLockExclusive(int fd) override { return inner_.tryLockExclusive(fd); }
    void unlock(int fd) noexcept override { inner_.unlock(fd); }
    void syncDirectory(const std::string& path) override { inner_.syncDirectory(path); }
    void makeDirectory(const std::string& path) override { inner_.makeDirectory(path); }
    bool directoryExists(const std::string& path) override { return inner_.directoryExists(path); }
    std::vector<std::string> listDirectory(const std::string& path) override {
        return inner_.listDirectory(path);
    }

protected:
    io::IoBackend& inner() { return inner_; }

private:
    io::IoBackend& inner_;
};

class FaultBackend final : public ForwardingBackend {
public:
    using ForwardingBackend::ForwardingBackend;

    // How many writes to let through before the next one is bent. Negative
    // means let everything through.
    int writes_until_fault = -1;
    // Bytes of the faulting write that land before it fails. Zero makes the
    // write fail without touching the file; a value in the middle of a
    // record is what a disk that fills up partway leaves behind.
    std::size_t partial_bytes = 0;
    // Fail every write at this offset and no others. The frontier lives at a
    // fixed offset in the header page, so this picks it out without needing
    // to know which descriptor it rides.
    std::uint64_t fail_writes_at = kNoOffset;
    bool fail_fsync = false;

    static constexpr std::uint64_t kNoOffset = ~0ull;

    int writes = 0;
    int faulted_writes = 0;
    int fsyncs = 0;
    std::vector<std::string> directory_syncs;

    void writeAll(int fd, iovec* iov, int count, std::uint64_t offset) override {
        ++writes;
        const bool by_offset = fail_writes_at != kNoOffset && offset == fail_writes_at;
        const bool by_count = writes_until_fault >= 0 && writes_until_fault-- == 0;
        if (!by_offset && !by_count) {
            ForwardingBackend::writeAll(fd, iov, count, offset);
            return;
        }
        ++faulted_writes;
        if (partial_bytes != 0) {
            writePrefix(fd, iov, count, offset, partial_bytes);
        }
        throw std::system_error(EIO, std::system_category(), "injected write failure");
    }

    void fsync(int fd) override {
        ++fsyncs;
        if (fail_fsync) {
            throw std::system_error(EIO, std::system_category(), "injected fsync failure");
        }
        ForwardingBackend::fsync(fd);
    }

    void syncDirectory(const std::string& path) override {
        directory_syncs.push_back(path);
        ForwardingBackend::syncDirectory(path);
    }

private:
    // Writes the first `bytes` the iovecs describe and no more, so the file
    // is left holding a piece of a record.
    void writePrefix(int fd, iovec* iov, int count, std::uint64_t offset, std::size_t bytes) {
        std::vector<iovec> prefix;
        std::size_t left = bytes;
        for (int i = 0; i < count && left != 0; ++i) {
            iovec one = iov[i];
            if (one.iov_len > left) {
                one.iov_len = left;
            }
            left -= one.iov_len;
            prefix.push_back(one);
        }
        if (!prefix.empty()) {
            ForwardingBackend::writeAll(fd, prefix.data(), static_cast<int>(prefix.size()), offset);
        }
    }
};

}  // namespace raf::test
