#pragma once
#include "poolcommon/uint.h"
#include "poolcommon/utils.h"
#include "poolcommon/timeTypes.h"
#include "idltool/jsonReadError.h"
#include "idltool/jsonReadString.h"
#include "idltool/jsonReadInt.h"
#include "idltool/jsonWriteString.h"
#include "idltool/jsonWriteInt.h"

// --- usertypes: direct Parse/Serialize ---

// uint256: UInt<256> as 64-char hex string (no 0x prefix)
inline JsonReadError __uint256Parse(const char *&p, const char *end, UInt<256> &out) {
  std::string value;
  auto error = jsonReadStringValue(p, end, value);
  if (error != JsonReadError::Ok)
    return error;
  if (value.size() != 64)
    return JsonReadError::UnexpectedChar;
  for (char c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return JsonReadError::UnexpectedChar;
  }
  out = UInt<256>::fromHex(value.c_str());
  return JsonReadError::Ok;
}

inline void __uint256Serialize(xmstream &out, const UInt<256> &value) {
  jsonWriteString(out, value.getHex());
}

// unixTime: Timestamp as int64
inline JsonReadError __unixTimeParse(const char *&p, const char *end, Timestamp &out) {
  int64_t value;
  auto error = jsonReadInt64(p, end, value);
  if (error != JsonReadError::Ok)
    return error;
  out = Timestamp::fromUnixTime(value);
  return JsonReadError::Ok;
}

inline void __unixTimeSerialize(xmstream &out, const Timestamp &value) {
  jsonWriteInt(out, value.toUnixTime());
}

// moneyBTC: UInt<384> as decimal string with hardcoded precision 8
inline JsonReadError __moneyBTCParse(const char *&p, const char *end, UInt<384> &out) {
  std::string value;
  auto error = jsonReadStringValue(p, end, value);
  if (error != JsonReadError::Ok)
    return error;
  if (!parseMoneyValue(value.c_str(), 8, &out))
    return JsonReadError::UnexpectedChar;
  return JsonReadError::Ok;
}

inline void __moneyBTCSerialize(xmstream &out, const UInt<384> &value) {
  jsonWriteString(out, FormatMoney(value, 8));
}

// --- derived: Resolve/Serialize with context ---

// money: UInt<384> from decimal string with variable precision (context = fractional digits)
inline bool __moneyResolve(const std::string &raw, uint32_t fractionalPart, UInt<384> &out) {
  return parseMoneyValue(raw.c_str(), fractionalPart, &out);
}

inline void __moneySerialize(xmstream &out, const UInt<384> &value, uint32_t fractionalPart) {
  jsonWriteString(out, FormatMoney(value, fractionalPart));
}
