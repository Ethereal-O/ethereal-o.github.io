#include "store/read_window.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>


namespace raf::store {
namespace {

std::uint64_t alignDown(std::uint64_t value, std::size_t to) {
    return value / to * to;
}

std::size_t alignUp(std::size_t value, std::size_t to) {
    return (value + to - 1) / to * to;
}

iovec makeIov(void* base, std::size_t length) {
    iovec iov{};
    iov.iov_base = base;
    iov.iov_len = length;
    return iov;
}

}  // namespace

ReadWindow::ReadWindow(std::size_t readahead) : readahead_(alignUp(readahead, kSector)) {
    reserve(readahead_ < kSector ? kSector : readahead_);
}

ReadWindow::~ReadWindow() {
    std::free(buffer_);
}

void ReadWindow::reserve(std::size_t bytes) {
    if (bytes <= capacity_) {
        return;
    }
    // Aligned to a page rather than to kSector: it satisfies every device,
    // and posix_memalign wants a multiple of sizeof(void*) anyway.
    void* raw = nullptr;
    if (::posix_memalign(&raw, 4096, bytes) != 0) {
        throw std::bad_alloc();
    }
    std::free(buffer_);
    buffer_ = static_cast<char*>(raw);
    capacity_ = bytes;
    length_ = 0;
}

std::size_t ReadWindow::read(io::IoBackend& backend, int fd, std::uint64_t offset, void* out,
                             std::size_t length, std::size_t fetch_at_least,
                             std::uint64_t anchor) {
    if (length == 0) {
        return 0;
    }
    if (!covers(offset, length)) {
        const std::uint64_t from = (anchor != kNoAnchor && anchor <= offset) ? anchor : offset;
        const std::uint64_t start = alignDown(from, kSector);
        // Big enough for the request even when one record is larger than the
        // window, which is what makes a record of any size readable without
        // a second path for the oversized case.
        const std::size_t needed =
            alignUp(static_cast<std::size_t>(offset - start) + length, kSector);
        // A hint replaces the standing readahead rather than competing with
        // it. The caller that passes one knows the record size; the
        // readahead is a guess made before anything had been read.
        const std::size_t target = (fetch_at_least != 0) ? fetch_at_least : readahead_;
        const std::size_t want = std::max(needed, alignUp(target, kSector));
        reserve(want);
        // `want`, not the buffer's capacity: the buffer may have been grown
        // by an earlier oversized record and reading it full again would
        // move that much for every record after it.
        iovec iov = makeIov(buffer_, want);
        const std::size_t got = backend.readSome(fd, &iov, 1, start);
        start_ = start;
        length_ = got;
    }
    const std::size_t at = static_cast<std::size_t>(offset - start_);
    if (at >= length_) {
        return 0;
    }
    const std::size_t available = std::min(length, length_ - at);
    std::memcpy(out, buffer_ + at, available);
    return available;
}

}  // namespace raf::store
