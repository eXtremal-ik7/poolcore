#pragma once
#include "poolcommon/uint.h"
#include "poolcommon/utils.h"
#include "poolcommon/timeTypes.h"

// money: UInt<384> <-> string, with uint32 context (fractional part size)
inline bool __moneyResolve(const std::string &raw, uint32_t fractionalPart, UInt<384> &out) {
  return parseMoneyValue(raw.c_str(), fractionalPart, &out);
}

inline std::string __moneyFormat(const UInt<384> &val, uint32_t fractionalPart) {
  return FormatMoney(val, fractionalPart);
}

// moneyBTC: UInt<384> <-> string, hardcoded precision 8 (no context)
inline bool __moneyBTCResolve(const std::string &raw, UInt<384> &out) {
  return parseMoneyValue(raw.c_str(), 8, &out);
}

inline std::string __moneyBTCFormat(const UInt<384> &val) {
  return FormatMoney(val, 8);
}

// unixTime: Timestamp <-> int64, no context
inline bool __unixTimeResolve(int64_t raw, Timestamp &out) {
  out = Timestamp::fromUnixTime(raw);
  return true;
}

inline int64_t __unixTimeFormat(const Timestamp &val) {
  return val.toUnixTime();
}
