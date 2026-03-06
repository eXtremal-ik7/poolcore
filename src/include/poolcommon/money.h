#pragma once
#include "poolcommon/uint.h"
#include "poolcommon/utils.h"

inline bool __moneyResolve(const std::string &raw, uint32_t fractionalPart, UInt<384> &out) {
  return parseMoneyValue(raw.c_str(), fractionalPart, &out);
}

inline std::string __moneyFormat(const UInt<384> &val, uint32_t fractionalPart) {
  return FormatMoney(val, fractionalPart);
}
