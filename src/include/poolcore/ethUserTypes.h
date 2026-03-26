#pragma once
#include "poolcommon/uint.h"
#include "idltool/jsonReadError.h"
#include "idltool/jsonReadString.h"
#include "idltool/jsonWriteString.h"
#include <cinttypes>

// weiHex: UInt<384> from "0x"-prefixed hex string, value stored in upper 128 bits (shifted left by 256)
inline JsonReadError __weiHexParse(const char *&p, const char *end, UInt<384> &out) {
  std::string value;
  auto error = jsonReadStringValue(p, end, value);
  if (error != JsonReadError::Ok)
    return error;
  if (value.size() < 3 || value[0] != '0' || value[1] != 'x')
    return JsonReadError::UnexpectedChar;
  out = UInt<384>::fromHex(value.c_str() + 2) << 256;
  return JsonReadError::Ok;
}

inline void __weiHexSerialize(std::string &out, const UInt<384> &value) {
  jsonWriteString(out, (value >> 256).getHex(false, true, false));
}

// uint256x: UInt<256> from "0x"-prefixed 64-char LE hex string (Ethereum hash format)
inline JsonReadError __uint256xParse(const char *&p, const char *end, UInt<256> &out) {
  std::string value;
  auto error = jsonReadStringValue(p, end, value);
  if (error != JsonReadError::Ok)
    return error;
  if (value.size() != 66 || value[0] != '0' || value[1] != 'x')
    return JsonReadError::UnexpectedChar;
  out = UInt<256>::fromHex(value.c_str() + 2);
  return JsonReadError::Ok;
}

inline void __uint256xSerialize(std::string &out, const UInt<256> &value) {
  jsonWriteString(out, value.getHex(false, true, false));
}

// uint64Hex: uint64_t from "0x"-prefixed hex string
inline JsonReadError __uint64HexParse(const char *&p, const char *end, uint64_t &out) {
  std::string value;
  auto error = jsonReadStringValue(p, end, value);
  if (error != JsonReadError::Ok)
    return error;
  if (value.size() < 3 || value[0] != '0' || value[1] != 'x')
    return JsonReadError::UnexpectedChar;
  out = strtoull(value.c_str() + 2, nullptr, 16);
  return JsonReadError::Ok;
}

inline void __uint64HexSerialize(std::string &out, uint64_t value) {
  char buf[32];
  snprintf(buf, sizeof(buf), "\"0x%" PRIx64 "\"", value);
  out.append(buf);
}
