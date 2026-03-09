// JSON value parser — readBool
#pragma once

#include "idltool/jsonReadError.h"
#include <cstring>

inline JsonReadError jsonReadBool(const char *&p, const char *end, bool &out) {
  if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
    p += 4;
    out = true;
    return JsonReadError::Ok;
  }
  if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
    p += 5;
    out = false;
    return JsonReadError::Ok;
  }
  if (p >= end)
    return JsonReadError::UnexpectedEnd;
  return JsonReadError::UnexpectedChar;
}
