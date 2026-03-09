// JSON value parser — readInt64 + readInt32
#pragma once

#include "idltool/jsonReadError.h"

inline JsonReadError jsonReadInt64(const char *&p, const char *end, int64_t &out) {
  if (p >= end) return JsonReadError::UnexpectedEnd;
  const char *start = p;
  bool negative = false;
  if (*p == '-') { negative = true; p++; }
  if (p >= end || (unsigned)(*p - '0') >= 10) {
    p = start; return JsonReadError::UnexpectedChar;
  }
  uint64_t val = 0;
  const char *digitStart = p;
  while (p < end && (unsigned)(*p - '0') < 10) {
    val = val * 10 + (*p - '0');
    p++;
  }
  int nDigits = (int)(p - digitStart);
  if (__builtin_expect(nDigits <= 18, 1)) {
    // <= 18 digits: no overflow possible
  } else if (nDigits == 19) {
    if (negative) {
      if (val > 9223372036854775808ULL) { p = start; return JsonReadError::Overflow; }
    } else {
      if (val > 9223372036854775807ULL) { p = start; return JsonReadError::Overflow; }
    }
  } else {
    p = start; return JsonReadError::Overflow;
  }
  out = negative ? -static_cast<int64_t>(val) : static_cast<int64_t>(val);
  return JsonReadError::Ok;
}

inline JsonReadError jsonReadInt32(const char *&p, const char *end, int32_t &out) {
  int64_t v;
  auto err = jsonReadInt64(p, end, v);
  if (err != JsonReadError::Ok) return err;
  if (v < INT32_MIN || v > INT32_MAX) return JsonReadError::RangeError;
  out = (int32_t)v;
  return JsonReadError::Ok;
}
