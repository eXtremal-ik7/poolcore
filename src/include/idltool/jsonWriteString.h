// Generated JSON write helper — jsonWriteString
#pragma once

#include "p2putils/xmstream.h"
#include <string_view>
#include <cstdint>

inline char jsonHexDigit(uint8_t b) {
  return b < 10 ? '0' + b : 'a' + b - 10;
}

inline void jsonWriteString(xmstream &out, std::string_view value) {
  out.write('"');
  for (char ch : value) {
    switch (ch) {
      case '"':  out.write("\\\""); break;
      case '\\': out.write("\\\\"); break;
      case '\b': out.write("\\b"); break;
      case '\f': out.write("\\f"); break;
      case '\n': out.write("\\n"); break;
      case '\r': out.write("\\r"); break;
      case '\t': out.write("\\t"); break;
      default:
        if (static_cast<uint8_t>(ch) < 0x20) {
          out.write("\\u00");
          out.write(jsonHexDigit(static_cast<uint8_t>(ch) >> 4));
          out.write(jsonHexDigit(static_cast<uint8_t>(ch) & 0xF));
        } else {
          out.write(ch);
        }
        break;
    }
  }
  out.write('"');
}
