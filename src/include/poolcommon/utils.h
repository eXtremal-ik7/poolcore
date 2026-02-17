#pragma once

#include <stdarg.h>
#include <string>
#include "poolcommon/uint.h"
#include "p2putils/strExtras.h"

static inline UInt<384> fromRational(uint64_t value) {
  return UInt<384>(value) << 256;
}

std::string FormatMoney(const UInt<384> &n, unsigned decimalDigits, bool fPlus=false);
std::string FormatMoneyFull(const UInt<384> &n, unsigned decimalDigits);
bool parseMoneyValue(const char *value, unsigned decimalDigits, UInt<384> *out);

static inline uint8_t hexDigit2bin(char c)
{
  uint8_t digit = c - '0';
  if (digit >= 10)
    digit -= ('A' - '0' - 10);
  if (digit >= 16)
    digit -= ('a' - 'A');
  return digit;
}

static inline char bin2hexLowerCaseDigit(uint8_t b)
{
  return b < 10 ? '0'+b : 'a'+b-10;
}

static inline void hex2bin(const char *in, size_t inSize, void *out)
{
  uint8_t *pOut = static_cast<uint8_t*>(out);
  for (size_t i = 0; i < inSize/2; i++)
    pOut[i] = (hexDigit2bin(in[i*2]) << 4) | hexDigit2bin(in[i*2+1]);
}

static inline void bin2hexLowerCase(const void *in, char *out, size_t size)
{
  const uint8_t *pIn = static_cast<const uint8_t*>(in);
  for (size_t i = 0, ie = size; i != ie; ++i) {
    out[i*2] = bin2hexLowerCaseDigit(pIn[i] >> 4);
    out[i*2+1] = bin2hexLowerCaseDigit(pIn[i] & 0xF);
  }
}

static inline std::string bin2hexLowerCase(const void *in, size_t size)
{
  std::string result;
  result.resize(size * 2);
  bin2hexLowerCase(in, result.data(), size);
  return result;
}

template<typename T>
std::string writeHexLE(T value, unsigned sizeInBytes)
{
  std::string result;
  for (unsigned i = 0; i < sizeInBytes; i++) {
    uint8_t byte = value & 0xFF;
    result.push_back(bin2hexLowerCaseDigit(byte >> 4));
    result.push_back(bin2hexLowerCaseDigit(byte & 0xF));
    value >>= 8;
  }

  return result;
}

template<typename T>
std::string writeHexBE(T value, unsigned sizeInBytes)
{
  std::string result;
  value = xswap(value);
  value >>= 8*(sizeof(T) - sizeInBytes);

  for (unsigned i = 0; i < sizeInBytes; i++) {
    uint8_t byte = value & 0xFF;
    result.push_back(bin2hexLowerCaseDigit(byte >> 4));
    result.push_back(bin2hexLowerCaseDigit(byte & 0xF));
    value >>= 8;
  }

  return result;
}

template<typename T>
T readHexBE(const char *data, unsigned size)
{
  T result = 0;
  for (unsigned i = 0; i < size; i++) {
    result <<= 8;
    result |= (hexDigit2bin(data[i*2]) << 4);
    result |= hexDigit2bin(data[i*2 + 1]);
  }

  return result;
}

template<typename T>
void writeBinBE(T value, unsigned sizeInBytes, void *out)
{
  value = xswap(value);
  value >>= 8*(sizeof(T) - sizeInBytes);
  uint8_t *p = static_cast<uint8_t*>(out);
  for (size_t i = 0; i < sizeInBytes; i++) {
    p[i] = value & 0xFF;
    value >>= 8;
  }
}

