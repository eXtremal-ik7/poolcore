// Generated JSON scanner — readUInt64 + readUInt32 definitions
#pragma once

#include "idltool/jsonScanner.h"

template<bool Verbose, bool Comments>
bool JsonScannerImpl<Verbose, Comments>::readUInt64(uint64_t &out) {
  skipWhitespace();
  if (p >= end) { setError("expected integer, got end of input"); return false; }
  const char *start = p;
  if (*p == '-') { setError("expected unsigned integer, got negative value"); return false; }
  if ((unsigned)(*p - '0') >= 10) {
    setError(std::string("expected integer, got '") + *p + "'"); return false;
  }
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
      p = start; setError("integer overflow"); return false;
    }
  } else {
    p = start; setError("integer overflow"); return false;
  }
  out = val;
  return true;
}

template<bool Verbose, bool Comments>
bool JsonScannerImpl<Verbose, Comments>::readUInt32(uint32_t &out) {
  uint64_t v;
  if (!readUInt64(v)) return false;
  if (v > UINT32_MAX) { setError("integer out of uint32 range"); return false; }
  out = (uint32_t)v;
  return true;
}
