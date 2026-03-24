// JSON write helper — jsonWriteHexData (vector<uint8_t> → hex string)
#pragma once

#include <string>
#include <vector>
#include <cstdint>

inline void jsonWriteHexData(std::string &out, const std::vector<uint8_t> &data) {
  static const char hex[] = "0123456789abcdef";
  out += '"';
  for (uint8_t b : data) {
    out += hex[b >> 4];
    out += hex[b & 0x0f];
  }
  out += '"';
}
