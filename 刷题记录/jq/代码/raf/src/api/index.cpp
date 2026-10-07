#include <raf/raf.h>

#include <stdexcept>

#include "store/format.h"

namespace raf {

std::uint32_t segmentOf(INDEX_TYPE index) {
    if (index < 0) {
        throw std::logic_error("raf: negative record index");
    }
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(index) >> format::kSegmentShift);
}

std::uint64_t offsetOf(INDEX_TYPE index) {
    if (index < 0) {
        throw std::logic_error("raf: negative record index");
    }
    return static_cast<std::uint64_t>(index) & format::kOffsetMask;
}

}  // namespace raf
