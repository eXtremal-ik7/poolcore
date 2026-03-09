// JSON value parser error codes
#pragma once

#include <cstdint>

enum class JsonReadError : uint8_t {
  Ok = 0,
  UnexpectedEnd,  // p >= end when value expected
  UnexpectedChar, // first character invalid for this type
  Overflow,       // numeric value too large for type (int64/uint64)
  RangeError,     // narrowing failed (int32/uint32 range)
  Unterminated,   // string: missing closing quote
};
