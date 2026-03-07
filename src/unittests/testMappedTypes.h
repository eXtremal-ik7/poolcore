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

inline bool __aValResolve(const std::string &raw, uint32_t ctx, int64_t &out)
{
  return __testValResolve(raw, ctx, out);
}

inline std::string __aValFormat(int64_t val, uint32_t ctx)
{
  return __testValFormat(val, ctx);
}

inline bool __bValResolve(const std::string &raw, uint32_t ctx, int64_t &out)
{
  return __testValResolve(raw, ctx, out);
}

inline std::string __bValFormat(int64_t val, uint32_t ctx)
{
  return __testValFormat(val, ctx);
}

// Context-free mapped type: int64_t <-> decimal string
inline bool __plainValResolve(const std::string &raw, int64_t &out)
{
  char *end;
  out = strtoll(raw.c_str(), &end, 10);
  return end != raw.c_str() && *end == '\0';
}

inline std::string __plainValFormat(int64_t val)
{
  return std::to_string(val);
}

// Context-free mapped type: uint64_t <-> uint64 (identity, for testing wire type dispatch)
inline bool __counterValResolve(uint64_t raw, uint64_t &out)
{
  out = raw;
  return true;
}

inline uint64_t __counterValFormat(uint64_t val)
{
  return val;
}

inline bool __counter32ValResolve(uint32_t raw, uint32_t &out)
{
  out = raw;
  return true;
}

inline uint32_t __counter32ValFormat(uint32_t val)
{
  return val;
}

inline bool __ctxCounter32ValResolve(uint32_t raw, uint32_t ctx, uint32_t &out)
{
  out = raw + ctx;
  return true;
}

inline uint32_t __ctxCounter32ValFormat(uint32_t val, uint32_t ctx)
{
  return val + ctx;
}
