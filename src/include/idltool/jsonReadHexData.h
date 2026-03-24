// JSON value parser — readHexData (hex string → vector<uint8_t>)
#pragma once

#include "idltool/jsonReadError.h"
#include <vector>
#include <cstdint>

inline JsonReadError jsonReadHexData(const char *&p, const char *end, std::vector<uint8_t> &out) {
  if (p >= end)
    return JsonReadError::UnexpectedEnd;
  if (*p != '"')
    return JsonReadError::UnexpectedChar;
  p++;
  const char *start = p;
  while (p < end && *p != '"') p++;
  if (p >= end)
    return JsonReadError::Unterminated;
  size_t len = p - start;
  if (len % 2 != 0)
    return JsonReadError::UnexpectedChar;
  out.resize(len / 2);
  for (size_t i = 0; i < len; i += 2) {
    auto hexVal = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return 10 + c - 'a';
      if (c >= 'A' && c <= 'F') return 10 + c - 'A';
      return -1;
    };
    int hi = hexVal(start[i]);
    int lo = hexVal(start[i + 1]);
    if (hi < 0 || lo < 0)
      return JsonReadError::UnexpectedChar;
    out[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
  }
  p++; // skip closing quote
  return JsonReadError::Ok;
}
