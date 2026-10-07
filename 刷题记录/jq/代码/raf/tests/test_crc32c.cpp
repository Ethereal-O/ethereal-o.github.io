#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

#include "common/crc32c.h"

namespace raf {
namespace {

// CRC-32C from its definition, a bit at a time, sharing no table and no code
// path with the implementation. The fast path folds three interleaved
// checksums back together with precomputed GF(2) shifts, which is exactly the
// kind of arithmetic that can be subtly wrong while still looking plausible,
// so it gets checked against this.
std::uint32_t referenceCrc32c(const void* data, std::size_t length) {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0x82f63b78u & (~(crc & 1u) + 1u));
        }
    }
    return ~crc;
}

// Content that varies byte to byte, so a fold that drops or misplaces a slice
// cannot go unnoticed.
std::string patterned(std::size_t length) {
    std::string value(length, '\0');
    for (std::size_t i = 0; i < length; ++i) {
        value[i] = static_cast<char>((i * 131u) ^ (i >> 5) ^ 0x3cu);
    }
    return value;
}

TEST(Crc32c, MatchesTheCastagnoliCheckValues) {
    EXPECT_EQ(crc32c("", 0), 0x00000000u);
    EXPECT_EQ(crc32c("a", 1), 0xc1d04330u);
    EXPECT_EQ(crc32c("abc", 3), 0x364b3fb7u);
    EXPECT_EQ(crc32c("123456789", 9), 0xe3069283u);

    const std::string sentence = "The quick brown fox jumps over the lazy dog";
    EXPECT_EQ(crc32c(sentence.data(), sentence.size()), 0x22620404u);
}

TEST(Crc32c, CoversEveryByteValue) {
    std::vector<unsigned char> bytes(256);
    std::iota(bytes.begin(), bytes.end(), 0);
    EXPECT_EQ(crc32c(bytes.data(), bytes.size()), 0x9c44184bu);
}

// The reader checksums a record's metadata and payload without ever putting
// them in the same buffer, so splitting a checksum has to be exact.
TEST(Crc32c, SplittingTheInputDoesNotChangeTheResult) {
    std::vector<unsigned char> bytes(1280);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<unsigned char>(i);
    }
    const std::uint32_t whole = crc32c(bytes.data(), bytes.size());

    for (const std::size_t split : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8},
                                    std::size_t{100}, std::size_t{1279}, bytes.size()}) {
        const std::uint32_t first = crc32c(bytes.data(), split);
        EXPECT_EQ(crc32cExtend(first, bytes.data() + split, bytes.size() - split), whole)
            << "split at " << split;
    }
}

// The fast path only engages past a block, and stitches a tail on afterwards,
// so the lengths that matter are the ones around those boundaries.
TEST(Crc32c, MatchesTheDefinitionAtEveryLength) {
    const std::string data = patterned(16384);
    std::vector<std::size_t> lengths;
    for (std::size_t n = 0; n <= 300; ++n) {
        lengths.push_back(n);
    }
    // Around each multiple of the interleaved block size, and a few beyond.
    for (std::size_t block = 1024; block <= 12288; block += 1024) {
        for (int delta = -9; delta <= 9; ++delta) {
            lengths.push_back(block + delta);
        }
    }
    for (const std::size_t n : {std::size_t{3071}, std::size_t{3072}, std::size_t{3073},
                                std::size_t{6143}, std::size_t{6144}, std::size_t{6145},
                                std::size_t{9216}, std::size_t{12345}, std::size_t{16384}}) {
        lengths.push_back(n);
    }

    for (const std::size_t n : lengths) {
        ASSERT_LE(n, data.size());
        EXPECT_EQ(crc32c(data.data(), n), referenceCrc32c(data.data(), n)) << "length " << n;
    }
}

// Splitting has to survive the fast path too: a record's payload arrives in
// whatever pieces the reads gave it.
TEST(Crc32c, SplittingAcrossTheInterleavedBlockIsExact) {
    const std::string data = patterned(12288);
    const std::uint32_t whole = referenceCrc32c(data.data(), data.size());
    for (const std::size_t split : {std::size_t{1}, std::size_t{1023}, std::size_t{1024},
                                    std::size_t{3071}, std::size_t{3072}, std::size_t{3073},
                                    std::size_t{6144}, std::size_t{9217}, std::size_t{12287}}) {
        const std::uint32_t first = crc32c(data.data(), split);
        EXPECT_EQ(crc32cExtend(first, data.data() + split, data.size() - split), whole)
            << "split at " << split;
    }
}

TEST(Crc32c, DetectsBitFlipsAnywhereInALargeBuffer) {
    const std::string data = patterned(12288);
    const std::uint32_t original = crc32c(data.data(), data.size());
    // One position inside each interleaved slice of the first block, one in a
    // later block, one in the tail: a fold that mixed up two slices would
    // still checksum correctly if only one slice were ever exercised.
    for (const std::size_t position :
         {std::size_t{0}, std::size_t{500}, std::size_t{1024}, std::size_t{1500}, std::size_t{2048},
          std::size_t{2500}, std::size_t{3072}, std::size_t{7000}, std::size_t{12287}}) {
        std::string damaged = data;
        damaged[position] = static_cast<char>(damaged[position] ^ 0x20);
        EXPECT_NE(crc32c(damaged.data(), damaged.size()), original) << "at " << position;
    }
}

// Two payloads that differ only by a swap of whole slices must not collide --
// the check that the fold weights each slice by its real position.
TEST(Crc32c, NoticesSlicesSwappedAround) {
    const std::string data = patterned(3072);
    std::string swapped = data;
    std::swap_ranges(swapped.begin(), swapped.begin() + 1024, swapped.begin() + 1024);
    EXPECT_NE(crc32c(swapped.data(), swapped.size()), crc32c(data.data(), data.size()));
    EXPECT_EQ(crc32c(swapped.data(), swapped.size()),
              referenceCrc32c(swapped.data(), swapped.size()));
}

TEST(Crc32c, DetectsSingleBitFlips) {
    std::string value(4096, 'x');
    for (std::size_t i = 0; i < value.size(); i += 97) {
        value[i] = static_cast<char>(i);
    }
    const std::uint32_t original = crc32c(value.data(), value.size());
    for (const std::size_t position :
         {std::size_t{0}, std::size_t{1}, std::size_t{2047}, std::size_t{4095}}) {
        std::string damaged = value;
        damaged[position] = static_cast<char>(damaged[position] ^ 0x08);
        EXPECT_NE(crc32c(damaged.data(), damaged.size()), original) << "at " << position;
    }
}

TEST(Crc32c, HandlesLengthsAroundTheWordLoop) {
    std::string value(64, '\0');
    for (std::size_t i = 0; i < value.size(); ++i) {
        value[i] = static_cast<char>('A' + (i % 26));
    }
    // Every path through the unrolled loops -- eight-byte blocks, a four-byte
    // tail, single bytes -- has to agree with the byte-at-a-time definition.
    for (std::size_t length = 0; length <= value.size(); ++length) {
        std::uint32_t expected = 0;
        for (std::size_t i = 0; i < length; ++i) {
            expected = crc32cExtend(expected, value.data() + i, 1);
        }
        EXPECT_EQ(crc32c(value.data(), length), expected) << "length " << length;
    }
}

}  // namespace
}  // namespace raf
