// Generated JSON write helper — jsonWriteInt
#pragma once

#include "p2putils/xmstream.h"
#include <cstdint>

inline void jsonWriteInt(xmstream &out, int64_t v) {
  char buf[21];
  char *p = buf + sizeof(buf);
  uint64_t uv;
  if (v < 0) {
    uv = -static_cast<uint64_t>(v);
  } else if (v == 0) {
    out.write('0');
    return;
  } else {
    uv = static_cast<uint64_t>(v);
  }
  while (uv > 0) { *--p = '0' + static_cast<char>(uv % 10); uv /= 10; }
  if (v < 0) *--p = '-';
  out.write(p, buf + sizeof(buf) - p);
}
