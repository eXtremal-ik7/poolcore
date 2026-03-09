// Generated JSON scanner — extracted from idltool codegen
#pragma once

#include <string>
#include <cstring>
#include <cstdint>

#ifndef IDLTOOL_PARSE_ERROR_DEFINED
#define IDLTOOL_PARSE_ERROR_DEFINED
struct ParseError {
  int row = 0;
  int col = 0;
  std::string message;
};
#endif

#include "idltool/jsonReadError.h"
#include "idltool/jsonReadInt.h"
#include "idltool/jsonReadUInt.h"
#include "idltool/jsonReadDouble.h"
#include "idltool/jsonReadBool.h"
#include "idltool/jsonReadString.h"

template<bool Verbose, bool Comments = false> struct JsonScannerImpl {
  const char *p;
  const char *end;
  const char *begin = nullptr;
  ParseError *error = nullptr;

  void computePosition(int &row, int &col) const {
    row = 1;
    col = 1;
    for (const char *c = begin; c < p; c++) {
      if (*c == '\n') {
        row++;
        col = 1;
      } else
        col++;
    }
  }

  void setError([[maybe_unused]] const std::string &msg) {
    if constexpr (Verbose) {
      if (!error || !error->message.empty())
        return;
      computePosition(error->row, error->col);
      error->message = msg;
    }
  }

  void formatReadError([[maybe_unused]] JsonReadError err, [[maybe_unused]] const char *field, [[maybe_unused]] const char *typeName) {
    if constexpr (Verbose) {
      if (!error || !error->message.empty())
        return;
      computePosition(error->row, error->col);
      switch (err) {
        case JsonReadError::UnexpectedEnd: error->message = std::string("field '") + field + "': expected " + typeName + ", got end of input"; break;
        case JsonReadError::UnexpectedChar: error->message = std::string("field '") + field + "': expected " + typeName + ", got '" + *p + "'"; break;
        case JsonReadError::Overflow: error->message = std::string("field '") + field + "': " + typeName + " overflow"; break;
        case JsonReadError::RangeError: error->message = std::string("field '") + field + "': " + typeName + " out of range"; break;
        case JsonReadError::Unterminated: error->message = std::string("field '") + field + "': unterminated string"; break;
        default: break;
      }
    }
  }

  __attribute__((always_inline)) void skipWhitespace() {
    while (p < end && (unsigned char)(*p - 1) < ' ') p++;
    if constexpr (Comments) {
      if (__builtin_expect(p + 1 < end && p[0] == '/', 0)) {
        skipComments();
      }
    }
  }

  __attribute__((noinline)) void skipComments() {
    do {
      if (p[1] == '/') {
        p += 2;
        while (p < end && *p != '\n') p++;
      } else if (p[1] == '*') {
        p += 2;
        while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
        if (p + 1 < end)
          p += 2;
      } else
        return;
      while (p < end && (unsigned char)(*p - 1) < ' ') p++;
    } while (p + 1 < end && p[0] == '/');
  }

  __attribute__((always_inline)) bool expectChar(char c) {
    skipWhitespace();
    if (p < end && *p == c) {
      p++;
      return true;
    }
    expectCharError(c);
    return false;
  }

  __attribute__((always_inline)) bool expectCharNoWs(char c) {
    if (p < end && *p == c) {
      p++;
      return true;
    }
    expectCharError(c);
    return false;
  }

  __attribute__((noinline)) void expectCharError(char c) {
    if (p >= end)
      setError(std::string("expected '") + c + "', got end of input");
    else
      setError(std::string("expected '") + c + "', got '" + *p + "'");
  }

  bool readString(const char *&str, size_t &len) {
    skipWhitespace();
    if (p >= end) {
      setError("expected string, got end of input");
      return false;
    }
    if (*p != '"') {
      setError(std::string("expected string, got '") + *p + "'");
      return false;
    }
    p++;
    str = p;
    while (p < end && *p != '"') {
      if (*p == '\\') {
        p++;
        if (p < end)
          p++;
        else {
          setError("unterminated string");
          return false;
        }
      } else
        p++;
    }
    if (p >= end) {
      setError("unterminated string");
      return false;
    }
    len = p - str;
    p++;
    return true;
  }

  bool readStringHash(const char *&str, size_t &len, uint32_t &hash, uint32_t seed, uint32_t mult) {
    skipWhitespace();
    if (p >= end) {
      setError("expected string, got end of input");
      return false;
    }
    if (*p != '"') {
      setError(std::string("expected string, got '") + *p + "'");
      return false;
    }
    p++;
    str = p;
    uint32_t h = seed;
    while (p < end && *p != '"') {
      if (*p == '\\') {
        h = h * mult + (unsigned char)*p;
        p++;
        if (p < end) {
          h = h * mult + (unsigned char)*p;
          p++;
        } else {
          setError("unterminated string");
          return false;
        }
      } else {
        h = h * mult + (unsigned char)*p;
        p++;
      }
    }
    if (p >= end) {
      setError("unterminated string");
      return false;
    }
    len = p - str;
    hash = h;
    p++;
    return true;
  }

  bool readNull() {
    if (end - p >= 4 && memcmp(p, "null", 4) == 0) {
      p += 4;
      return true;
    }
    return false;
  }

  bool skipValue() {
    skipWhitespace();
    if (p >= end) {
      setError("unexpected end of input");
      return false;
    }
    switch (*p) {
      case '"': {
        const char *s;
        size_t l;
        return readString(s, l);
      }
      case '{': {
        p++;
        skipWhitespace();
        if (p < end && *p == '}') {
          p++;
          return true;
        }
        for (;;) {
          const char *s;
          size_t l;
          if (!readString(s, l))
            return false;
          if (!expectChar(':'))
            return false;
          if (!skipValue())
            return false;
          skipWhitespace();
          if (p < end && *p == ',') {
            p++;
            continue;
          }
          break;
        }
        return expectChar('}');
      }
      case '[': {
        p++;
        skipWhitespace();
        if (p < end && *p == ']') {
          p++;
          return true;
        }
        for (;;) {
          if (!skipValue())
            return false;
          skipWhitespace();
          if (p < end && *p == ',') {
            p++;
            continue;
          }
          break;
        }
        return expectChar(']');
      }
      case 't': return (end - p >= 4 && memcmp(p, "true", 4) == 0) ? (p += 4, true) : (setError("invalid literal"), false);
      case 'f': return (end - p >= 5 && memcmp(p, "false", 5) == 0) ? (p += 5, true) : (setError("invalid literal"), false);
      case 'n': return (end - p >= 4 && memcmp(p, "null", 4) == 0) ? (p += 4, true) : (setError("invalid literal"), false);
      default: {
        if (*p == '-')
          p++;
        if (p >= end || (*p < '0' || *p > '9')) {
          setError("expected value");
          return false;
        }
        while (p < end && *p >= '0' && *p <= '9') p++;
        if (p < end && *p == '.') {
          p++;
          while (p < end && *p >= '0' && *p <= '9') p++;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
          p++;
          if (p < end && (*p == '+' || *p == '-'))
            p++;
          while (p < end && *p >= '0' && *p <= '9') p++;
        }
        return true;
      }
    }
  }

  // Forwarding methods — call free functions from jsonRead*.h
  JsonReadError readStringValue(std::string &out) { return jsonReadStringValue(p, end, out); }
  JsonReadError readInt64(int64_t &out) { return jsonReadInt64(p, end, out); }
  JsonReadError readInt32(int32_t &out) { return jsonReadInt32(p, end, out); }
  JsonReadError readUInt64(uint64_t &out) { return jsonReadUInt64(p, end, out); }
  JsonReadError readUInt32(uint32_t &out) { return jsonReadUInt32(p, end, out); }
  JsonReadError readDouble(double &out) { return jsonReadDouble(p, end, out); }
  JsonReadError readBool(bool &out) { return jsonReadBool(p, end, out); }
};

using JsonScanner = JsonScannerImpl<false, false>;
using JsonScannerComments = JsonScannerImpl<false, true>;
using VerboseJsonScanner = JsonScannerImpl<true, false>;
using VerboseJsonScannerComments = JsonScannerImpl<true, true>;
