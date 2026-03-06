#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

// Test mapped type: int64_t <-> string, with uint32 context (scale factor)
// resolve: parse string as integer scaled by 10^ctx
// format: format integer scaled by 10^ctx

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

inline std::string __testValFormat(int64_t val, uint32_t ctx)
{
  int64_t scale = 1;
  for (uint32_t i = 0; i < ctx; i++)
    scale *= 10;
  if (ctx == 0)
    return std::to_string(val);
  int64_t whole = val / scale;
  int64_t frac = val % scale;
  if (frac < 0) frac = -frac;
  std::string result = std::to_string(whole) + ".";
  std::string fracStr = std::to_string(frac);
  while (fracStr.size() < ctx)
    fracStr = "0" + fracStr;
  return result + fracStr;
}
