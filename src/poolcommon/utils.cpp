#include "poolcommon/utils.h"
#include "poolcommon/uint.h"
#include <inttypes.h>

std::string FormatMoney(const UInt<384> &n, unsigned decimalDigits, bool fPlus)
{
  // Discard lower 256 bits (fractional satoshi), keep upper 128 bits (satoshi count)
  UInt<128> satoshi = UInt<128>::truncate(n >> 256);

  // Check for negative (MSB set means underflow/negative in two's complement)
  bool isNegative = satoshi.isNegative();
  if (isNegative)
    satoshi.negate();

  std::string result;
  result.reserve(64);

  if (isNegative)
    result.push_back('-');
  else if (fPlus)
    result.push_back('+');

  // Compute rationalPartSize = 10^decimalDigits
  UInt<128> rationalPartSize(1u);
  for (unsigned i = 0; i < decimalDigits; i++)
    rationalPartSize.mul64(10u);

  // Compute quotient (coins) and remainder (fractional coins in satoshi)
  UInt<128> quotient;
  UInt<128> remainder;
  satoshi.divmod(rationalPartSize, &quotient, &remainder);

  // Format integer part
  result += quotient.getDecimal();

  // Format fractional part (if any)
  if (!remainder.isZero()) {
    result.push_back('.');

    std::string fractional = remainder.getDecimal();

    // Add leading zeros
    for (size_t i = fractional.length(); i < decimalDigits; i++)
      result.push_back('0');

    // Remove trailing zeros from fracStr and append
    size_t lastNonZero = fractional.find_last_not_of('0');
    if (lastNonZero != std::string::npos)
      result += fractional.substr(0, lastNonZero + 1);
  }

  return result;
}

std::string FormatMoneyFull(const UInt<384> &n, unsigned decimalDigits)
{
  // Upper 128 bits = satoshi count, lower 256 bits = fractional satoshi
  UInt<128> satoshi = UInt<128>::truncate(n >> 256);
  UInt<256> frac256 = UInt<256>::truncate(n);

  bool isNegative = satoshi.isNegative();
  if (isNegative)
    satoshi.negate();

  std::string result;
  result.reserve(96);

  if (isNegative)
    result.push_back('-');

  // Compute rationalPartSize = 10^decimalDigits
  UInt<128> rationalPartSize(1u);
  for (unsigned i = 0; i < decimalDigits; i++)
    rationalPartSize.mul64(10u);

  // Compute quotient (coins) and remainder (fractional coins in satoshi)
  UInt<128> quotient;
  UInt<128> remainder;
  satoshi.divmod(rationalPartSize, &quotient, &remainder);

  result += quotient.getDecimal();
  result.push_back('.');

  // Fractional coins part (fixed width = decimalDigits)
  {
    std::string fractional = remainder.getDecimal();
    for (size_t i = fractional.length(); i < decimalDigits; i++)
      result.push_back('0');
    result += fractional;
  }

  // Sub-satoshi part: multiply frac256 by 10^16 and shift right 256
  {
    UInt<384> wide(frac256);
    for (unsigned i = 0; i < 16; i++)
      wide *= 10u;
    uint64_t subSatoshi = (wide >> 256).low64();

    char buf[17];
    snprintf(buf, sizeof(buf), "%016" PRIu64, subSatoshi);
    result += buf;
  }

  // Remove trailing zeros after the decimal point
  size_t lastNonZero = result.find_last_not_of('0');
  if (lastNonZero != std::string::npos && result[lastNonZero] == '.')
    lastNonZero--;
  result.resize(lastNonZero + 1);

  return result;
}

bool parseMoneyValue(const char *value, unsigned decimalDigits, UInt<384> *out)
{
  *out = UInt<384>();

  // Compute rationalPartSize = 10^decimalDigits
  UInt<128> rationalPartSize(1u);
  for (unsigned i = 0; i < decimalDigits; i++)
    rationalPartSize.mul64(10u);

  UInt<128> fractionalMultiplier = rationalPartSize;
  UInt<128> integerPart;
  UInt<128> fractionalPart;
  const char *p = value;
  char s;

  // Parse integer part (coins)
  if (*p == 0)
    return false;
  for (;; p++) {
    s = *p;
    if (s >= '0' && s <= '9') {
      integerPart.mul64(10u);
      integerPart.add64(static_cast<unsigned>(s - '0'));
    } else if (s == '.') {
      break;
    } else if (s == '\0') {
      // No fractional part: result = integerPart * rationalPartSize
      integerPart.mul(rationalPartSize);
      *out = UInt<384>(integerPart) << 256;
      return true;
    } else {
      return false;
    }
  }

  // Parse fractional part
  p++;
  for (;; p++) {
    s = *p;
    if (s >= '0' && s <= '9') {
      fractionalPart.mul64(10u);
      fractionalPart.add64(static_cast<unsigned>(s - '0'));
      fractionalMultiplier.divmod64(10u);
      if (fractionalMultiplier.isZero())
        return false;
    } else if (s == '\0') {
      break;
    } else {
      return false;
    }
  }

  // Result in satoshi = integerPart * rationalPartSize + fractionalPart * fractionalMultiplier
  integerPart.mul(rationalPartSize);
  fractionalPart.mul(fractionalMultiplier);
  integerPart.add(fractionalPart);
  *out = UInt<384>(integerPart) << 256;
  return true;
}
