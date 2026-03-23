#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include "idltool/jsonWriteString.h"
#include "idltool/jsonWriteInt.h"
#include "idltool/jsonWriteUInt.h"

// testVal: int64_t from decimal string with scale factor (context = number of decimal places)
inline bool __testValResolve(const std::string &raw, uint32_t ctx, int64_t &out)
{
  char *end;
  double d = strtod(raw.c_str(), &end);
  if (end == raw.c_str())
    return false;
  int64_t scale = 1;
  for (uint32_t i = 0; i < ctx; i++)
    scale *= 10;
  out = (int64_t)(d * scale);
  return true;
}

inline void __testValSerialize(xmstream &out, int64_t val, uint32_t ctx)
{
  int64_t scale = 1;
  for (uint32_t i = 0; i < ctx; i++)
    scale *= 10;
  if (ctx == 0) {
    jsonWriteString(out, std::to_string(val));
    return;
  }
  int64_t whole = val / scale;
  int64_t frac = val % scale;
  if (frac < 0) frac = -frac;
  std::string result = std::to_string(whole) + ".";
  std::string fracStr = std::to_string(frac);
  while (fracStr.size() < ctx)
    fracStr = "0" + fracStr;
  jsonWriteString(out, result + fracStr);
}

// aVal: alias for testVal
inline bool __aValResolve(const std::string &raw, uint32_t ctx, int64_t &out)
{
  return __testValResolve(raw, ctx, out);
}

inline void __aValSerialize(xmstream &out, int64_t val, uint32_t ctx)
{
  __testValSerialize(out, val, ctx);
}

// bVal: alias for testVal
inline bool __bValResolve(const std::string &raw, uint32_t ctx, int64_t &out)
{
  return __testValResolve(raw, ctx, out);
}

inline void __bValSerialize(xmstream &out, int64_t val, uint32_t ctx)
{
  __testValSerialize(out, val, ctx);
}

// ctxCounter32Val: uint32_t with uint32 context (adds context to value)
inline bool __ctxCounter32ValResolve(uint32_t raw, uint32_t ctx, uint32_t &out)
{
  out = raw + ctx;
  return true;
}

inline void __ctxCounter32ValSerialize(xmstream &out, uint32_t val, uint32_t ctx)
{
  jsonWriteUInt(out, val + ctx);
}
