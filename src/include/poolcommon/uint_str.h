#pragma once

#include "uint.h"
#include <string>

template<unsigned Bits>
std::string uint2Hex(const UInt<Bits> &number, bool leadingZeroes = true, bool zeroxPrefix = false, bool upperCase = false)
{
  char data[Bits/4 + 8];
  number.toHex(data, leadingZeroes, zeroxPrefix, upperCase);
  return data;
}
