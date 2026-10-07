#include "common/crc32c.h"

#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h>
#define RAF_HAS_SSE42_CRC 1
#endif

namespace raf {
namespace {

// Bit-reflected Castagnoli polynomial, the same convention as the SSE4.2
// instruction, so both paths produce identical checksums.
constexpr std::uint32_t kPoly = 0x82f63b78u;

struct Table {
    std::uint32_t slice[8][256];

    constexpr Table() : slice{} {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ (kPoly & (~(crc & 1u) + 1u));
            }
            slice[0][i] = crc;
        }
        // Slicing by eight: slice[n][i] is the contribution of byte value i
        // once it has been shifted n bytes further into the message.
        for (std::uint32_t i = 0; i < 256; ++i) {
            for (int n = 1; n < 8; ++n) {
                slice[n][i] = (slice[n - 1][i] >> 8) ^ slice[0][slice[n - 1][i] & 0xffu];
            }
        }
    }
};

constexpr Table kTable;

// Bytes each of the three interleaved checksum chains covers per block. The
// hardware crc32 instruction retires one per cycle but takes three to produce
// its answer, so a single chain -- where every instruction waits on the one
// before it -- leaves two thirds of the throughput on the floor. Three chains
// running over three slices of the block keep it busy.
constexpr std::size_t kChainBytes = 1024;
static_assert(kChainBytes % 8 == 0);

// Advancing a checksum state over kChainBytes zero bytes, which is how the
// three chains get stitched back together. The step is linear over GF(2), so
// the whole advance is too, and a linear map on 32 bits is four byte-indexed
// tables.
struct ShiftTable {
    std::uint32_t part[4][256];

    constexpr explicit ShiftTable(std::size_t zeros) : part{} {
        for (int k = 0; k < 4; ++k) {
            for (std::uint32_t b = 0; b < 256; ++b) {
                std::uint32_t x = b << (8 * k);
                for (std::size_t i = 0; i < zeros; ++i) {
                    x = (x >> 8) ^ kTable.slice[0][x & 0xffu];
                }
                part[k][b] = x;
            }
        }
    }

    constexpr std::uint32_t operator()(std::uint32_t x) const {
        return part[0][x & 0xffu] ^ part[1][(x >> 8) & 0xffu] ^ part[2][(x >> 16) & 0xffu] ^
               part[3][x >> 24];
    }
};

constexpr ShiftTable kShift(kChainBytes);

std::uint32_t loadLe32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

std::uint32_t softwareCrc(std::uint32_t crc, const std::uint8_t* p, std::size_t n) {
    while (n >= 8) {
        const std::uint32_t lo = loadLe32(p) ^ crc;
        const std::uint32_t hi = loadLe32(p + 4);
        crc = kTable.slice[7][lo & 0xffu] ^ kTable.slice[6][(lo >> 8) & 0xffu] ^
              kTable.slice[5][(lo >> 16) & 0xffu] ^ kTable.slice[4][lo >> 24] ^
              kTable.slice[3][hi & 0xffu] ^ kTable.slice[2][(hi >> 8) & 0xffu] ^
              kTable.slice[1][(hi >> 16) & 0xffu] ^ kTable.slice[0][hi >> 24];
        p += 8;
        n -= 8;
    }
    while (n-- > 0) {
        crc = (crc >> 8) ^ kTable.slice[0][(crc ^ *p++) & 0xffu];
    }
    return crc;
}

#if RAF_HAS_SSE42_CRC
__attribute__((target("sse4.2"))) std::uint32_t hardwareCrc(std::uint32_t crc,
                                                            const std::uint8_t* p, std::size_t n) {
    // Three chains over three slices of the block, folded back together
    // afterwards. Chain 0 carries the incoming state; the other two start from
    // zero and get shifted into place, which is what makes the answer
    // identical to running a single chain straight through.
    constexpr std::size_t kBlock = 3 * kChainBytes;
    while (n >= kBlock) {
        std::uint32_t c0 = crc;
        std::uint32_t c1 = 0;
        std::uint32_t c2 = 0;
        for (std::size_t i = 0; i < kChainBytes; i += 8) {
            std::uint64_t w0;
            std::uint64_t w1;
            std::uint64_t w2;
            std::memcpy(&w0, p + i, sizeof(w0));
            std::memcpy(&w1, p + kChainBytes + i, sizeof(w1));
            std::memcpy(&w2, p + 2 * kChainBytes + i, sizeof(w2));
            c0 = static_cast<std::uint32_t>(_mm_crc32_u64(c0, w0));
            c1 = static_cast<std::uint32_t>(_mm_crc32_u64(c1, w1));
            c2 = static_cast<std::uint32_t>(_mm_crc32_u64(c2, w2));
        }
        crc = kShift(kShift(c0)) ^ kShift(c1) ^ c2;
        p += kBlock;
        n -= kBlock;
    }
    while (n >= 8) {
        std::uint64_t word;
        std::memcpy(&word, p, sizeof(word));
        crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, word));
        p += 8;
        n -= 8;
    }
    if (n >= 4) {
        crc = _mm_crc32_u32(crc, loadLe32(p));
        p += 4;
        n -= 4;
    }
    while (n-- > 0) {
        crc = _mm_crc32_u8(crc, *p++);
    }
    return crc;
}
#endif

}  // namespace

std::uint32_t crc32cExtend(std::uint32_t crc, const void* data, std::size_t length) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t state = ~crc;
#if RAF_HAS_SSE42_CRC
    // Asked on every call rather than cached in a global: the check is a load
    // and a test against a table GCC fills in before main, which is nothing
    // next to the checksum itself, and it keeps the answer from depending on
    // static initialisation order.
    state = __builtin_cpu_supports("sse4.2") ? hardwareCrc(state, p, length)
                                             : softwareCrc(state, p, length);
#else
    state = softwareCrc(state, p, length);
#endif
    return ~state;
}

std::uint32_t crc32c(const void* data, std::size_t length) {
    return crc32cExtend(0, data, length);
}

}  // namespace raf
