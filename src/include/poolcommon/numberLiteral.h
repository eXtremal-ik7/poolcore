#pragma once

#include "idltool/jsonReadError.h"
#include <string>

// numberLiteral: reads a JSON number token as std::string (no precision loss)
inline JsonReadError __numberLiteralParse(const char *&p, const char *end, std::string &out) {
  const char *start = p;
  if (p < end && *p == '-')
    p++;
  if (p >= end || *p < '0' || *p > '9')
    return JsonReadError::UnexpectedChar;
  while (p < end && *p >= '0' && *p <= '9')
    p++;
  if (p < end && *p == '.') {
    p++;
    if (p >= end || *p < '0' || *p > '9')
      return JsonReadError::UnexpectedChar;
    while (p < end && *p >= '0' && *p <= '9')
      p++;
  }
  if (p < end && (*p == 'e' || *p == 'E')) {
    p++;
    if (p < end && (*p == '+' || *p == '-'))
      p++;
    if (p >= end || *p < '0' || *p > '9')
      return JsonReadError::UnexpectedChar;
    while (p < end && *p >= '0' && *p <= '9')
      p++;
  }
  out.assign(start, p);
  return JsonReadError::Ok;
}

inline void __numberLiteralSerialize(std::string &out, const std::string &value) {
  out.append(value);
}
