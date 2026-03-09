// Generated JSON write helper — jsonWriteUInt
#pragma once

#include "p2putils/xmstream.h"
#include <cstdint>

inline void jsonWriteUInt(xmstream &out, uint64_t v) {
  if (v == 0) {
    out.write('0');
    return;
  }
  char buf[20];
  char *p = buf + sizeof(buf);
  while (v > 0) {
    *--p = '0' + static_cast<char>(v % 10);
    v /= 10;
  }
  out.write(p, buf + sizeof(buf) - p);
}
