#pragma once

#include <cstddef>
#include <cstdint>

namespace raf {

// CRC-32C (Castagnoli, polynomial 0x1EDC6F41), the checksum the on-disk format
// uses. Runs on the SSE4.2 crc32 instruction where the CPU has it and falls
// back to a table otherwise.
std::uint32_t crc32c(const void* data, std::size_t length);

// Continues a running checksum, so a record's metadata and payload can be
// covered without copying them next to each other.
std::uint32_t crc32cExtend(std::uint32_t crc, const void* data, std::size_t length);

}  // namespace raf
