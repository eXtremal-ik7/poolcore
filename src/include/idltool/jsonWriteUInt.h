// Generated JSON write helper — jsonWriteUInt
#pragma once

#include <string>
#include <cstdint>

inline void jsonWriteUInt(std::string &out, uint64_t v) {
  if (v == 0) {
    out += '0';
    return;
  }
  char buf[20];
  char *p = buf + sizeof(buf);
  while (v > 0) {
    *--p = '0' + static_cast<char>(v % 10);
    v /= 10;
  }
  out.append(p, buf + sizeof(buf) - p);
}
