// JSON write helper — jsonWriteHexData (vector<uint8_t> → hex string)
#pragma once

#include "p2putils/xmstream.h"
#include <vector>
#include <cstdint>

inline void jsonWriteHexData(xmstream &out, const std::vector<uint8_t> &data) {
  static const char hex[] = "0123456789abcdef";
  out.write('"');
  for (uint8_t b : data) {
    out.write(hex[b >> 4]);
    out.write(hex[b & 0x0f]);
  }
  out.write('"');
}
