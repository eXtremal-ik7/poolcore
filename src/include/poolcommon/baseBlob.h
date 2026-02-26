#pragma once

#include "endiantools.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <format>
#include <string>

template<unsigned Bits>
class BaseBlob {
public:
  BaseBlob() { setNull(); }
  BaseBlob(const BaseBlob &r) { memcpy(Data_, r.Data_, Size); }

  BaseBlob &operator=(const BaseBlob &r) {
    memcpy(Data_, r.Data_, Size);
    return *this;
  }

  static BaseBlob zero() {
    BaseBlob result;
    result.setNull();
    return result;
  }

  static BaseBlob fromHexRaw(const char *in) {
    BaseBlob result;
    result.setHexRaw(in);
    return result;
  }

  static BaseBlob fromHexLE(const char *in) {
    BaseBlob result;
    result.setHexLE(in);
    return result;
  }

  void setNull() { memset(Data_, 0, Size); }

  bool isNull() const {
    for (size_t i = 0; i < Size; ++i) {
      if (Data_[i] != 0)
        return false;
    }
    return true;
  }

  void setHexRaw(const char *in) {
    if (in[0] == '0' && in[1] == 'x')
      in += 2;
    for (size_t i = 0; i < Size; ++i) {
      Data_[i] = (decodeHex(in[i * 2]) << 4) | decodeHex(in[i * 2 + 1]);
    }
  }

  void setHexLE(const char *in) {
    if (in[0] == '0' && in[1] == 'x')
      in += 2;
    for (size_t i = 0; i < Size; ++i) {
      Data_[Size - 1 - i] = (decodeHex(in[i * 2]) << 4) | decodeHex(in[i * 2 + 1]);
    }
  }

  void getHexRaw(char *buf, bool zeroxPrefix = false, bool upperCase = false) const {
    size_t offset = 0;
    if (zeroxPrefix) {
      buf[0] = '0';
      buf[1] = 'x';
      offset = 2;
    }
    for (size_t i = 0; i < Size; ++i) {
      buf[offset + i * 2]     = encodeHex(Data_[i] >> 4, upperCase);
      buf[offset + i * 2 + 1] = encodeHex(Data_[i] & 0x0F, upperCase);
    }
    buf[offset + Size * 2] = 0;
  }

  void getHexLE(char *buf, bool zeroxPrefix = false, bool upperCase = false) const {
    size_t offset = 0;
    if (zeroxPrefix) {
      buf[0] = '0';
      buf[1] = 'x';
      offset = 2;
    }
    for (size_t i = 0; i < Size; ++i) {
      uint8_t c = Data_[Size - 1 - i];
      buf[offset + i * 2]     = encodeHex(c >> 4, upperCase);
      buf[offset + i * 2 + 1] = encodeHex(c & 0x0F, upperCase);
    }
    buf[offset + Size * 2] = 0;
  }

  std::string getHexRaw(bool upperCase = false, bool zeroxPrefix = false) const {
    std::string result;
    result.resize(Size * 2 + (zeroxPrefix ? 2 : 0));
    getHexRaw(result.data(), zeroxPrefix, upperCase);
    return result;
  }

  std::string getHexLE(bool upperCase = false, bool zeroxPrefix = false) const {
    std::string result;
    result.resize(Size * 2 + (zeroxPrefix ? 2 : 0));
    getHexLE(result.data(), zeroxPrefix, upperCase);
    return result;
  }

  int cmp(const BaseBlob &r) const { return memcmp(Data_, r.Data_, Size); }

  bool operator<(const BaseBlob &r) const { return cmp(r) < 0; }
  bool operator>(const BaseBlob &r) const { return cmp(r) > 0; }
  bool operator==(const BaseBlob &r) const { return cmp(r) == 0; }

  uint8_t *begin() { return Data_; }
  uint8_t *end() { return Data_ + Size; }
  const uint8_t *begin() const { return Data_; }
  const uint8_t *end() const { return Data_ + Size; }
  size_t size() const { return Size; }

  uint64_t get64(int pos) const {
    size_t offset = pos * 8;
    if (offset + 8 <= Size) {
      return readle(*reinterpret_cast<const uint64_t*>(Data_ + offset));
    }
    // Побайтовое чтение когда данных меньше 8 байт
    uint64_t result = 0;
    for (size_t i = 0; i + offset < Size && i < 8; ++i) {
      result |= static_cast<uint64_t>(Data_[offset + i]) << (i * 8);
    }
    return result;
  }

private:
  static constexpr size_t Size = Bits / 8;
  static_assert(Bits % 8 == 0);

private:
  uint8_t Data_[Size];

private:
  static char encodeHex(uint8_t value, bool upperCase) {
    return value < 10 ? '0'+value : (upperCase ? 'A'+value-10 : 'a'+value-10);
  }

  static uint8_t decodeHex(char c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    else if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    else
      return c - 'a' + 10;
  }
};

template<unsigned Bits>
struct std::hash<BaseBlob<Bits>> {
  size_t operator()(const BaseBlob<Bits> &s) const {
    return s.get64(0);
  }
};

template<unsigned int BitSize>
struct TbbHash {
  size_t hash(const BaseBlob<BitSize> &s) const {
    return s.get64(0);
  }

  bool equal(const BaseBlob<BitSize> &s1, const BaseBlob<BitSize> &s2) const {
    return s1 == s2;
  }
};

// std::format support for BaseBlob<Bits>
// Default (no spec or {:x}) = hex raw lowercase
// {:X} = hex uppercase, {:#x}/{:#X} = hex with 0x prefix
template<unsigned Bits>
struct std::formatter<BaseBlob<Bits>> {
  bool altForm_ = false;
  bool upperCase_ = false;

  template <class ParseContext>
  constexpr auto parse(ParseContext &ctx) {
    auto it = ctx.begin();
    if (it != ctx.end() && *it == '#') {
      altForm_ = true;
      ++it;
    }
    if (it != ctx.end() && (*it == 'x' || *it == 'X')) {
      upperCase_ = (*it == 'X');
      ++it;
    }
    return it;
  }

  template <class FormatContext>
  auto format(const BaseBlob<Bits> &value, FormatContext &ctx) const {
    return std::format_to(ctx.out(), "{}", value.getHexRaw(upperCase_, altForm_));
  }
};
