// A sliding aligned window over one segment file.
//
// O_DIRECT puts three conditions on every read: the file offset, the length
// and the buffer address all have to be multiples of the device's logical
// block size. raf's records satisfy none of them -- a payload sits at
// 32*(k+1) bytes past a sector boundary -- so a direct read has to cover an
// aligned superset of what was asked for and hand back the slice inside it.
//
// Covering an aligned superset per record would answer the alignment rules
// and nothing else. Iteration reads records in order, so the window is as
// large as a readahead would be and the records that follow come out of it
// without another system call. That is the whole of raf's readahead under
// O_DIRECT: a measured 1 MiB window reads at the drive's ceiling, and no
// cache is kept beyond it -- once the window slides past, the bytes are
// gone.
//
// One window belongs to one iteration cursor. Random reads do not use one:
// they have no locality to exploit and share a segment across threads.
#pragma once

#include <cstddef>
#include <cstdint>

#include "io/io_backend.h"

namespace raf::store {

class ReadWindow {
public:
    // The device's logical block size. 512 is the smallest any current drive
    // reports and is a legal multiple of every larger one, so aligning to it
    // satisfies 4096-byte-sector devices too.
    static constexpr std::size_t kSector = 512;
    static constexpr std::size_t kDefaultCapacity = 1u << 20;

    // `readahead` is how much a refill fetches when the caller asks for
    // less. Zero means fetch exactly what was asked, rounded out to sectors,
    // which is what a reader with no locality wants: a window that always
    // fills itself to capacity turns a 4 KiB lookup into a 64 KiB read.
    explicit ReadWindow(std::size_t readahead = kDefaultCapacity);
    ~ReadWindow();

    ReadWindow(const ReadWindow&) = delete;
    ReadWindow& operator=(const ReadWindow&) = delete;

    // Copies up to `length` bytes from `offset` into `out` and returns how
    // many it copied. A short return means the file ends there, exactly as
    // IoBackend::readSome does.
    // `fetch_at_least` replaces the configured readahead for this call,
    // for a caller that knows what it is about to ask for next -- a random
    // lookup reads a header and then the payload behind it, and fetching
    // both at once is the difference between one system call and two.
    // `anchor` is where a refill should start, when that is not simply
    // `offset`. Iteration passes the first byte of the record it is reading,
    // so a window that has to be refilled lands on a record boundary and
    // holds a whole number of records; refilled from the payload's offset
    // instead, it would start thirty-two bytes into one record and end
    // thirty-two bytes into another, and every refill after it would inherit
    // that.
    std::size_t read(io::IoBackend& backend, int fd, std::uint64_t offset, void* out,
                     std::size_t length, std::size_t fetch_at_least = 0,
                     std::uint64_t anchor = kNoAnchor);

    static constexpr std::uint64_t kNoAnchor = ~0ull;

    // Forgets what is held, for a caller that knows the file moved under it.
    void invalidate() { length_ = 0; }

private:
    bool covers(std::uint64_t offset, std::size_t length) const {
        return length_ != 0 && offset >= start_ && offset + length <= start_ + length_;
    }
    void reserve(std::size_t bytes);

    char* buffer_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t readahead_ = 0;
    std::uint64_t start_ = 0;
    std::size_t length_ = 0;  // bytes of `buffer_` that hold file contents
};

}  // namespace raf::store
