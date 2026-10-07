// Shared test scaffolding.
//
// Nothing here is random. Payloads are a pure function of (writer, sequence,
// size) and every rendezvous between threads or processes is a std::latch, so
// a run that fails fails the same way the next time.
#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/wait.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <raf/raf.h>

#include "store/archive.h"
#include "store/format.h"

namespace raf::test {

// A directory under /tmp that is removed when the test finishes.
// Where temporary archives go. Every segment is read with O_DIRECT, which
// tmpfs refuses outright, so /tmp is only usable where it is a real
// filesystem -- it is tmpfs under systemd's PrivateTmp, in plenty of
// containers, and on any machine that mounts it that way. Falling back to
// the working directory keeps the suite from failing for a reason that has
// nothing to do with raf.
inline bool supportsDirect(const std::string& dir) {
    const std::string probe = dir + "/.raf-direct-probe";
    const int fd = ::open(probe.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    const int direct = ::open(probe.c_str(), O_RDONLY | O_DIRECT);
    const bool ok = direct >= 0;
    if (ok) {
        ::close(direct);
    }
    ::unlink(probe.c_str());
    return ok;
}

inline const std::string& tempRoot() {
    static const std::string root = [] {
        for (const char* candidate : {"/tmp", "."}) {
            if (supportsDirect(candidate)) {
                return std::string(candidate);
            }
        }
        throw std::runtime_error("no directory here can open a file O_DIRECT");
    }();
    return root;
}

class TempArchive {
public:
    explicit TempArchive(const char* tag = "raf") {
        std::string pattern = tempRoot() + "/";
        pattern += tag;
        pattern += "-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (::mkdtemp(buffer.data()) == nullptr) {
            throw std::system_error(errno, std::system_category(), "mkdtemp");
        }
        root_ = buffer.data();
        path_ = root_ + "/archive";
    }

    ~TempArchive() { removeTree(root_); }

    TempArchive(const TempArchive&) = delete;
    TempArchive& operator=(const TempArchive&) = delete;

    // Path to hand to RecordWriter/RecordReader. It does not exist yet; a
    // writer creates it.
    const std::string& path() const { return path_; }
    const std::string& root() const { return root_; }

    static void removeTree(const std::string& path) {
        struct stat st {};
        if (::lstat(path.c_str(), &st) != 0) {
            return;
        }
        if (S_ISDIR(st.st_mode)) {
            for (const std::string& name : io::posixBackend().listDirectory(path)) {
                removeTree(path + "/" + name);
            }
            ::rmdir(path.c_str());
        } else {
            ::unlink(path.c_str());
        }
    }

private:
    std::string root_;
    std::string path_;
};

// Record payloads.
//
// A payload of 32 bytes or more starts with a fixed-width header naming the
// writer, the sequence number and the length, so a reader can say exactly
// which record it is holding. The rest is a byte pattern derived from those
// three numbers, which makes a wrong payload -- or a payload from the right
// record but the wrong length -- fail the comparison.
inline constexpr std::size_t kPayloadHeader = 32;

inline std::uint64_t payloadSeed(std::uint32_t writer, std::uint64_t sequence, std::size_t size) {
    std::uint64_t x = (static_cast<std::uint64_t>(writer) << 40) ^
                      (sequence * 0x9e3779b97f4a7c15ull) ^ (static_cast<std::uint64_t>(size) << 7);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

inline std::string makePayload(std::uint32_t writer, std::uint64_t sequence, std::size_t size) {
    std::string value(size, '\0');
    const std::uint64_t seed = payloadSeed(writer, sequence, size);
    for (std::size_t i = 0; i < size; ++i) {
        value[i] = static_cast<char>((seed >> ((i % 8) * 8)) ^ (i * 31u) ^ 0x5au);
    }
    if (size >= kPayloadHeader) {
        // Each field is masked to its own width, so the header is exactly
        // kPayloadHeader bytes whatever it is handed.
        char header[64];
        const int written =
            std::snprintf(header, sizeof(header), "W%06lluS%012lluL%010llu|",
                          static_cast<unsigned long long>(writer % 1000000ull),
                          static_cast<unsigned long long>(sequence % 1000000000000ull),
                          static_cast<unsigned long long>(size % 10000000000ull));
        (void)written;
        std::memcpy(value.data(), header, kPayloadHeader);
    }
    return value;
}

struct PayloadId {
    std::uint32_t writer;
    std::uint64_t sequence;
    std::size_t size;
};

// Recovers the identity a payload carries, or nullopt if it carries none.
inline std::optional<PayloadId> identifyPayload(const std::string& value) {
    if (value.size() < kPayloadHeader || value[0] != 'W') {
        return std::nullopt;
    }
    unsigned writer = 0;
    unsigned long long sequence = 0;
    unsigned long long size = 0;
    if (std::sscanf(value.c_str(), "W%6uS%12lluL%10llu|", &writer, &sequence, &size) != 3) {
        return std::nullopt;
    }
    return PayloadId{writer, sequence, static_cast<std::size_t>(size)};
}

// True if `value` is exactly the payload makePayload() would have produced for
// the identity it claims.
inline bool payloadIsIntact(const std::string& value) {
    const std::optional<PayloadId> id = identifyPayload(value);
    if (!id || id->size != value.size()) {
        return false;
    }
    return value == makePayload(id->writer, id->sequence, id->size);
}

// A rendezvous between a parent and one child process, over a pair of pipes.
//
// The child reports that it has reached a point -- carrying one byte of news,
// such as which segment it ended up with -- and blocks until the parent lets
// it go. Pipes rather than a shared latch because a latch is not something two
// processes can share portably, and rather than sleeps because a sleep is a
// guess.
class ChildGate {
public:
    ChildGate() {
        if (::pipe(ready_) != 0 || ::pipe(go_) != 0) {
            throw std::system_error(errno, std::system_category(), "pipe");
        }
    }

    ~ChildGate() {
        for (const int fd : {ready_[0], ready_[1], go_[0], go_[1]}) {
            ::close(fd);
        }
    }

    ChildGate(const ChildGate&) = delete;
    ChildGate& operator=(const ChildGate&) = delete;

    // Child side: announce arrival and wait to be released.
    void arrive(unsigned char news = 0) {
        writeByte(ready_[1], news);
        readByte(go_[0]);
    }

    // Parent side.
    unsigned char waitForChild() { return readByte(ready_[0]); }
    void release() { writeByte(go_[1], 0); }

private:
    static void writeByte(int fd, unsigned char value) {
        while (::write(fd, &value, 1) != 1) {
            if (errno != EINTR) {
                throw std::system_error(errno, std::system_category(), "write");
            }
        }
    }

    static unsigned char readByte(int fd) {
        unsigned char value = 0;
        while (true) {
            const ssize_t n = ::read(fd, &value, 1);
            if (n == 1) {
                return value;
            }
            if (n == 0) {
                throw std::logic_error("the other side of the gate went away");
            }
            if (errno != EINTR) {
                throw std::system_error(errno, std::system_category(), "read");
            }
        }
    }

    int ready_[2]{};
    int go_[2]{};
};

// Runs `body` in a child process and waits for it.
//
// The child leaves through _exit, so no destructor runs and nothing gets
// closed on the way out -- which is exactly the state a crash leaves an
// archive in. Returns the raw wait status.
inline int runInChild(const std::function<int()>& body) {  // NOLINT(misc-no-recursion)
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) {
        throw std::system_error(errno, std::system_category(), "fork");
    }
    if (pid == 0) {
        int code = 70;
        try {
            code = body();
        } catch (...) {
            code = 71;
        }
        std::fflush(nullptr);
        ::_exit(code);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::system_error(errno, std::system_category(), "waitpid");
        }
    }
    return status;
}

inline bool exitedWith(int status, int code) {
    return WIFEXITED(status) && WEXITSTATUS(status) == code;
}

inline bool killedBy(int status, int signal) {
    return WIFSIGNALED(status) && WTERMSIG(status) == signal;
}

// Reads an archive to the end and returns the records in iteration order.
inline std::vector<std::string> drain(RecordReader& reader) {
    std::vector<std::string> values;
    while (std::optional<std::string> value = reader.readNext()) {
        values.push_back(std::move(*value));
    }
    return values;
}

inline std::vector<std::string> drain(const std::string& path) {
    RecordReader reader(path);
    return drain(reader);
}

// Segment files in an archive, ordered by id.
inline std::vector<std::string> segmentPaths(const std::string& archive) {
    std::vector<std::string> paths;
    for (const std::uint32_t id : store::Archive::listSegments(io::posixBackend(), archive)) {
        paths.push_back(store::Archive::segmentPath(archive, id));
    }
    return paths;
}

// Raw file editing, for the tests that have to put damage on disk.
inline std::uint64_t fileSize(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        throw std::system_error(errno, std::system_category(), "stat " + path);
    }
    return static_cast<std::uint64_t>(st.st_size);
}

inline void writeRaw(const std::string& path, std::uint64_t offset, const void* data,
                     std::size_t length) {
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
        throw std::system_error(errno, std::system_category(), "open " + path);
    }
    const ssize_t n = ::pwrite(fd, data, length, static_cast<off_t>(offset));
    const int err = errno;
    ::fsync(fd);
    ::close(fd);
    if (n != static_cast<ssize_t>(length)) {
        throw std::system_error(err, std::system_category(), "pwrite " + path);
    }
}

inline std::string readRaw(const std::string& path, std::uint64_t offset, std::size_t length) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::system_error(errno, std::system_category(), "open " + path);
    }
    std::string buffer(length, '\0');
    const ssize_t n = ::pread(fd, buffer.data(), length, static_cast<off_t>(offset));
    const int err = errno;
    ::close(fd);
    if (n < 0) {
        throw std::system_error(err, std::system_category(), "pread " + path);
    }
    buffer.resize(static_cast<std::size_t>(n));
    return buffer;
}

// Flips one bit, which is the cheapest way to make a checksum fail.
inline void flipBit(const std::string& path, std::uint64_t offset) {
    std::string byte = readRaw(path, offset, 1);
    if (byte.empty()) {
        throw std::logic_error("flipBit past the end of " + path);
    }
    byte[0] = static_cast<char>(byte[0] ^ 0x01);
    writeRaw(path, offset, byte.data(), 1);
}

inline void truncateFile(const std::string& path, std::uint64_t length) {
    if (::truncate(path.c_str(), static_cast<off_t>(length)) != 0) {
        throw std::system_error(errno, std::system_category(), "truncate " + path);
    }
}

// Reads the record header at `offset` straight off disk.
inline format::RecordMeta readMeta(const std::string& path, std::uint64_t offset) {
    const std::string bytes = readRaw(path, offset, format::kMetaSize);
    format::RecordMeta meta{};
    if (bytes.size() != format::kMetaSize) {
        throw std::logic_error("short header read");
    }
    std::memcpy(&meta, bytes.data(), sizeof(meta));
    return meta;
}

}  // namespace raf::test
