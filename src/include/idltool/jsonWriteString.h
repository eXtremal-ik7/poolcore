// Generated JSON write helper — jsonWriteString
#pragma once

#include <string>
#include <string_view>
#include <cstdint>

inline char jsonHexDigit(uint8_t b) { return b < 10 ? '0' + b : 'a' + b - 10; }

inline void jsonWriteString(std::string &out, std::string_view value) {
  out += '"';
  const char *p = value.data();
  const char *end = p + value.size();
  while (p < end) {
    // Fast path: scan for characters that need escaping
    const char *start = p;
    while (p < end && static_cast<uint8_t>(*p) >= 0x20 && *p != '"' && *p != '\\')
      p++;
    if (p > start)
      out.append(start, p - start);
    if (p >= end)
      break;
    // Slow path: escape one character
    char ch = *p++;
    switch (ch) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        out.append("\\u00");
        out += jsonHexDigit(static_cast<uint8_t>(ch) >> 4);
        out += jsonHexDigit(static_cast<uint8_t>(ch) & 0xF);
        break;
    }
  }
  out += '"';
}
