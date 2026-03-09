// JSON value parser — readUInt64 + readUInt32
#pragma once

#include "idltool/jsonReadError.h"

inline JsonReadError jsonReadUInt64(const char *&p, const char *end, uint64_t &out) {
  if (p >= end) return JsonReadError::UnexpectedEnd;
  if (*p == '-') return JsonReadError::UnexpectedChar;
  if ((unsigned)(*p - '0') >= 10) return JsonReadError::UnexpectedChar;
  const char *start = p;
  uint64_t val = 0;
  const char *digitStart = p;
  while (p < end && (unsigned)(*p - '0') < 10) {
    val = val * 10 + (*p - '0');
    p++;
  }
  int nDigits = (int)(p - digitStart);
  if (__builtin_expect(nDigits <= 19, 1)) {
    // <= 19 digits: no overflow possible (max 19-digit = 9.9e18 < UINT64_MAX)
  } else if (nDigits == 20) {
    // 20 digits: valid only if first digit is '1' and no wrap-around
    if (*digitStart > '1' || val < 10000000000000000000ULL) {
      p = start; return JsonReadError::Overflow;
    }
  } else {
    p = start; return JsonReadError::Overflow;
  }
  out = val;
  return JsonReadError::Ok;
}

inline JsonReadError jsonReadUInt32(const char *&p, const char *end, uint32_t &out) {
  uint64_t v;
  auto err = jsonReadUInt64(p, end, v);
  if (err != JsonReadError::Ok) return err;
  if (v > UINT32_MAX) return JsonReadError::RangeError;
  out = (uint32_t)v;
  return JsonReadError::Ok;
}
