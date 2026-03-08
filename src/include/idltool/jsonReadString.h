// Generated JSON scanner — readStringValue definition
#pragma once

#include "idltool/jsonScanner.h"

template<bool Verbose, bool Comments>
bool JsonScannerImpl<Verbose, Comments>::readStringValue(std::string &out) {
  skipWhitespace();
  if (p >= end) { setError("expected string, got end of input"); return false; }
  if (*p != '"') { setError(std::string("expected string, got '") + *p + "'"); return false; }
  p++;
  // Fast path: scan for closing quote without escapes
  const char *start = p;
  while (p < end && *p != '"' && *p != '\\') p++;
  if (p < end && *p == '"') {
    out.assign(start, p - start);
    p++;
    return true;
  }
  // Slow path: has escapes or unterminated
  out.assign(start, p - start);
  while (p < end && *p != '"') {
    if (*p == '\\') {
      p++;
      if (p >= end) { setError("unterminated string"); return false; }
      switch (*p) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          if (p + 5 <= end) {
            auto hexVal = [](char c) -> int {
              if (c >= '0' && c <= '9') return c - '0';
              if (c >= 'a' && c <= 'f') return 10 + c - 'a';
              if (c >= 'A' && c <= 'F') return 10 + c - 'A';
              return -1;
            };
            int d0 = hexVal(p[1]), d1 = hexVal(p[2]), d2 = hexVal(p[3]), d3 = hexVal(p[4]);
            if (d0 >= 0 && d1 >= 0 && d2 >= 0 && d3 >= 0) {
              uint32_t cp = (d0 << 12) | (d1 << 8) | (d2 << 4) | d3;
              p += 4;
              if (cp >= 0xD800 && cp <= 0xDBFF && p + 7 <= end && p[1] == '\\' && p[2] == 'u') {
                int l0 = hexVal(p[3]), l1 = hexVal(p[4]), l2 = hexVal(p[5]), l3 = hexVal(p[6]);
                if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
                  uint32_t low = (l0 << 12) | (l1 << 8) | (l2 << 4) | l3;
                  if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    p += 6;
                  }
                }
              }
              if (cp < 0x80) {
                out += (char)cp;
              } else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
              } else if (cp < 0x10000) {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
              } else {
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
              }
            } else {
              out += '\\'; out += 'u';
            }
          } else {
            out += '\\'; out += 'u';
          }
          break;
        }
        default: out += *p; break;
      }
      p++;
    } else {
      out += *p++;
    }
  }
  if (p >= end) { setError("unterminated string"); return false; }
  p++;
  return true;
}
