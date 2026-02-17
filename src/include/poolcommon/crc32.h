#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// CRC-32C (Castagnoli), polynomial 0x1EDC6F41
// Check value for "123456789": 0xE3069283

#if defined(HAVE_SSE42)
#include <nmmintrin.h>

inline uint32_t crc32c(const void *data, size_t size) {
  const uint8_t *p = static_cast<const uint8_t*>(data);
  uint64_t crc = 0xFFFFFFFF;

  while (size >= 8) {
    uint64_t val;
    memcpy(&val, p, sizeof(val));
    crc = _mm_crc32_u64(crc, val);
    p += 8;
    size -= 8;
  }

  if (size >= 4) {
    uint32_t val;
    memcpy(&val, p, sizeof(val));
    crc = _mm_crc32_u32(static_cast<uint32_t>(crc), val);
    p += 4;
    size -= 4;
  }

  while (size--)
    crc = _mm_crc32_u8(static_cast<uint32_t>(crc), *p++);

  return static_cast<uint32_t>(crc) ^ 0xFFFFFFFF;
}

#else

inline constexpr uint32_t crc32cTableEntry(uint32_t index) {
  constexpr uint32_t poly = 0x82F63B78; // reflected 0x1EDC6F41
  uint32_t crc = index;
  for (int j = 0; j < 8; j++)
    crc = (crc & 1) ? (crc >> 1) ^ poly : crc >> 1;
  return crc;
}

inline constexpr std::array<uint32_t, 256> makeCrc32cTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; i++)
    table[i] = crc32cTableEntry(i);
  return table;
}

inline constexpr auto crc32cTable = makeCrc32cTable();

inline uint32_t crc32c(const void *data, size_t size) {
  const uint8_t *p = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < size; i++)
    crc = crc32cTable[static_cast<uint8_t>(crc ^ p[i])] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFF;
}

#endif
