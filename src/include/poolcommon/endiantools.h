#pragma once

#include <bit>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

consteval bool isLittleEndian() {
  return std::endian::native == std::endian::little;
}

consteval bool isBigEndian() {
  return std::endian::native == std::endian::big;
}

static inline uint16_t bswap(uint16_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap16(v);
#elif defined(_MSC_VER)
  return _byteswap_ushort(v);
#endif
}

static inline uint32_t bswap(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap32(v);
#elif defined(_MSC_VER)
  return _byteswap_ulong(v);
#endif
}

static inline uint64_t bswap(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap64(v);
#elif defined(_MSC_VER)
  return _byteswap_uint64(v);
#endif
}

template<typename T>
T readle(T value) {
  if constexpr (!isLittleEndian()) {
    return bswap(value);
  }
  return value;
}

template<typename T>
T writele(T value) {
  if constexpr (!isLittleEndian()) {
    return bswap(value);
  }
  return value;
}

template<typename T>
T readbe(T value) {
  if constexpr (isLittleEndian()) {
    return bswap(value);
  }
  return value;
}

template<typename T>
T writebe(T value) {
  if constexpr (isLittleEndian()) {
    return bswap(value);
  }
  return value;
}
