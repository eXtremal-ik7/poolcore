// Generated JSON scanner — readInt64 + readInt32 definitions
#pragma once

#include "idltool/jsonScanner.h"

template<bool Verbose, bool Comments>
bool JsonScannerImpl<Verbose, Comments>::readInt64(int64_t &out) {
  skipWhitespace();
  if (p >= end) { setError("expected integer, got end of input"); return false; }
  const char *start = p;
  bool negative = false;
  if (*p == '-') { negative = true; p++; }
  if (p >= end || (unsigned)(*p - '0') >= 10) {
    p = start; setError(std::string("expected integer, got '") + *start + "'"); return false;
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
      if (val > 9223372036854775808ULL) { p = start; setError("integer overflow"); return false; }
    } else {
      if (val > 9223372036854775807ULL) { p = start; setError("integer overflow"); return false; }
    }
  } else {
    p = start; setError("integer overflow"); return false;
  }
  out = negative ? -static_cast<int64_t>(val) : static_cast<int64_t>(val);
  return true;
}

template<bool Verbose, bool Comments>
bool JsonScannerImpl<Verbose, Comments>::readInt32(int32_t &out) {
  int64_t v;
  if (!readInt64(v)) return false;
  if (v < INT32_MIN || v > INT32_MAX) { setError("integer out of int32 range"); return false; }
  out = (int32_t)v;
  return true;
}
