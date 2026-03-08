// Generated JSON scanner — readBool definition
#pragma once

#include "idltool/jsonScanner.h"

template<bool Verbose, bool Comments>
bool JsonScannerImpl<Verbose, Comments>::readBool(bool &out) {
  skipWhitespace();
  if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
    p += 4; out = true; return true;
  }
  if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
    p += 5; out = false; return true;
  }
  if (p >= end)
    setError("expected boolean, got end of input");
  else
    setError(std::string("expected boolean, got '") + *p + "'");
  return false;
}
