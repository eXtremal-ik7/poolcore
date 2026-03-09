// JSON value parser — readDouble
#pragma once

#include "idltool/jsonReadError.h"

inline JsonReadError jsonReadDouble(const char *&p, const char *end, double &out) {
  if (p >= end)
    return JsonReadError::UnexpectedEnd;

  const char *start = p;
  bool negative = false;
  if (*p == '-') {
    negative = true;
    p++;
  }

  if (p >= end || *p < '0' || *p > '9') {
    p = start;
    return JsonReadError::UnexpectedChar;
  }

  uint64_t mantissa = 0;
  const char *intStart = p;
  while (p < end && (unsigned)(*p - '0') < 10) {
    mantissa = mantissa * 10 + (*p - '0');
    p++;
  }

  int intLen = (int)(p - intStart);
  int fracLen = 0;
  if (p < end && *p == '.') {
    p++;
    const char *fracStart = p;
    while (p < end && (unsigned)(*p - '0') < 10) {
      mantissa = mantissa * 10 + (*p - '0');
      p++;
    }
    fracLen = (int)(p - fracStart);
  }

  int exp;
  if (__builtin_expect(intLen + fracLen <= 19, 1)) {
    exp = -fracLen;
  } else {
    // >19 digits: reparse with 18-digit limit
    p = intStart;
    mantissa = 0;
    int digits = 0;
    while (p < end && (unsigned)(*p - '0') < 10) {
      if (digits < 18) {
        mantissa = mantissa * 10 + (*p - '0');
        digits++;
      }
      p++;
    }

    int intCaptured = digits;
    int fracCaptured = 0;
    int fracLeadZeros = 0;
    if (p < end && *p == '.') {
      p++;
      if (mantissa == 0) {
        digits = 0;
        intCaptured = 0;
        intLen = 0;
        while (p < end && *p == '0') {
          fracLeadZeros++;
          p++;
        }
      }
      while (p < end && (unsigned)(*p - '0') < 10) {
        if (digits < 18) {
          mantissa = mantissa * 10 + (*p - '0');
          digits++;
          fracCaptured++;
        }
        p++;
      }
    }

    exp = (intLen > intCaptured ? intLen - intCaptured : 0) - fracCaptured - fracLeadZeros;
  }

  if (p < end && (*p == 'e' || *p == 'E')) {
    p++;
    bool expNeg = false;
    if (p < end) {
      if (*p == '-') {
        expNeg = true;
        p++;
      } else if (*p == '+')
        p++;
    }
    int expVal = 0;
    while (p < end && (unsigned)(*p - '0') < 10) {
      expVal = expVal * 10 + (*p - '0');
      p++;
    }
    exp += expNeg ? -expVal : expVal;
  }

  static constexpr double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                      1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

  out = (double)mantissa;
  if (__builtin_expect(exp >= -22 && exp <= 22, 1)) {
    if (exp > 0)
      out *= kPow10[exp];
    else if (exp < 0)
      out /= kPow10[-exp];
  } else {
    while (exp > 0) {
      int step = exp > 22 ? 22 : exp;
      out *= kPow10[step];
      exp -= step;
    }
    while (exp < 0) {
      int step = exp < -22 ? 22 : -exp;
      out /= kPow10[step];
      exp += step;
    }
  }
  if (negative)
    out = -out;
  return JsonReadError::Ok;
}
