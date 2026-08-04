#pragma once

#include "p2putils/strExtras.h"
#include <stddef.h>

// xatoi and xitoah were dropped from p2putils in asyncio 0.5; kept here with the original semantics

template<typename Type> size_t xitoah(Type value, char *out, bool leadingZeroes, bool zeroxPrefix, bool upperCase)
{
  char *pOut = out;
  if (value < 0) {
    value = static_cast<Type>(0) - value;
    *pOut++ = '-';
  }

  if (zeroxPrefix) {
    *pOut++ = '0';
    *pOut++ = 'x';
  }

  bool leadingZeroFinished = false;
  for (size_t i = 0; i < sizeof(Type)*2; i++) {
    char digit = (value >> (sizeof(Type)*8 - 4)) & 0xFF;
    if (!leadingZeroes && digit == 0 && !leadingZeroFinished) {
      value <<= 4;
      continue;
    }

    if (digit < 10)
      digit += '0';
    else
      digit += (upperCase ? 'A' : 'a') - 10;

    *pOut++ = digit;
    value <<= 4;
    leadingZeroFinished = true;
  }

  if (!leadingZeroFinished)
    *pOut++ = '0';

  *pOut = 0;
  return pOut - out;
}

template<typename Type> Type xatoi(const char *in)
{
  int minus = 0;
  Type lvalue = 0;
  const char *p = in;
  if (*p == '-') {
    minus = 1;
    p++;
  } else if (*p == '+') {
    p++;
  }
  while (*p)
    lvalue = (lvalue*10) + (*p++ - '0');

  return minus ? static_cast<Type>(0)-lvalue : lvalue;
}
