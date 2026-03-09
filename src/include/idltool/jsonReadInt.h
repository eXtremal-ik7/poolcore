// JSON value parser — readInt64 + readInt32
#pragma once

#include "idltool/jsonReadError.h"
#include <limits>

inline JsonReadError jsonReadInt64(const char *&p, const char *end, int64_t &out) {
  if (p >= end)
    return JsonReadError::UnexpectedEnd;
  const char *start = p;
  bool negative = false;
  if (*p == '-') {
    negative = true;
    p++;
  }
  if (p >= end || (unsigned)(*p - '0') >= 10) {
    p = start;
    return JsonReadError::UnexpectedChar;
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
      if (val > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1) {
        p = start;
        return JsonReadError::Overflow;
      }
    } else {
      if (val > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        p = start;
        return JsonReadError::Overflow;
      }
    }
  } else {
    p = start;
    return JsonReadError::Overflow;
  }
  out = negative ? -static_cast<int64_t>(val) : static_cast<int64_t>(val);
  return JsonReadError::Ok;
}

inline JsonReadError jsonReadInt32(const char *&p, const char *end, int32_t &out) {
  int64_t v;
  auto err = jsonReadInt64(p, end, v);
  if (err != JsonReadError::Ok)
    return err;
  if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max())
    return JsonReadError::RangeError;
  out = (int32_t)v;
  return JsonReadError::Ok;
}
