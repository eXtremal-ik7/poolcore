// Generated JSON write helper — jsonWriteDouble
#pragma once

#include <string>
#include <cmath>
#include <cstdint>

inline void jsonWriteDouble(std::string &out, double v) {
  // Fast %.12g compatible formatter
  char buf[32];
  char *p = buf;

  if (v != v) {
    out.append("nan", 3);
    return;
  }
  if (v == 0.0) {
    if (std::signbit(v))
      out.append("-0", 2);
    else
      out += '0';
    return;
  }
  if (std::isinf(v)) {
    out.append(v < 0 ? "-inf" : "inf", v < 0 ? 4 : 3);
    return;
  }

  double av = v;
  if (v < 0) {
    *p++ = '-';
    av = -v;
  }

  // Find base-10 exponent via binary exponent
  int e2;
  frexp(av, &e2);
  int e10 = (int)floor((e2 - 1) * 0.30102999566398119);

  // Scale to 12 significant digits: 10^11 <= scaled < 10^12
  static constexpr double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                      1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  int shift = 11 - e10;
  double scaled = av;
  if (shift >= 0) {
    if (shift <= 22)
      scaled *= kPow10[shift];
    else {
      int s = shift;
      while (s > 0) {
        int step = s > 22 ? 22 : s;
        scaled *= kPow10[step];
        s -= step;
      }
    }
  } else {
    int ns = -shift;
    if (ns <= 22)
      scaled /= kPow10[ns];
    else {
      int s = ns;
      while (s > 0) {
        int step = s > 22 ? 22 : s;
        scaled /= kPow10[step];
        s -= step;
      }
    }
  }

  uint64_t digits = (uint64_t)llrint(scaled);

  // Correct off-by-one in e10
  if (digits >= 1000000000000ULL) {
    digits /= 10;
    e10++;
  } else if (digits < 100000000000ULL) {
    e10--;
    shift = 11 - e10;
    scaled = av;
    if (shift >= 0) {
      if (shift <= 22)
        scaled *= kPow10[shift];
      else {
        int s = shift;
        while (s > 0) {
          int step = s > 22 ? 22 : s;
          scaled *= kPow10[step];
          s -= step;
        }
      }
    } else {
      int ns = -shift;
      if (ns <= 22)
        scaled /= kPow10[ns];
      else {
        int s = ns;
        while (s > 0) {
          int step = s > 22 ? 22 : s;
          scaled /= kPow10[step];
          s -= step;
        }
      }
    }
    digits = (uint64_t)llrint(scaled);
    if (digits >= 1000000000000ULL) {
      digits /= 10;
      e10++;
    }
  }

  // Strip trailing zeros
  int nDigits = 12;
  while (nDigits > 1 && digits % 10 == 0) {
    digits /= 10;
    nDigits--;
  }

  // Extract digits into buffer (reverse order)
  char dbuf[12];
  for (int i = nDigits - 1; i >= 0; i--) {
    dbuf[i] = '0' + (int)(digits % 10);
    digits /= 10;
  }

  int intDigits = e10 + 1; // digits before decimal point

  if (e10 >= -4 && e10 < 12) {
    // Fixed notation
    if (intDigits >= nDigits) {
      // All digits before decimal, possibly pad with zeros
      memcpy(p, dbuf, nDigits);
      p += nDigits;
      for (int i = 0; i < intDigits - nDigits; i++) *p++ = '0';
    } else if (intDigits > 0) {
      // Some before, some after decimal
      memcpy(p, dbuf, intDigits);
      p += intDigits;
      *p++ = '.';
      memcpy(p, dbuf + intDigits, nDigits - intDigits);
      p += nDigits - intDigits;
    } else {
      // All after decimal with leading zeros
      *p++ = '0';
      *p++ = '.';
      for (int i = 0; i < -intDigits; i++) *p++ = '0';
      memcpy(p, dbuf, nDigits);
      p += nDigits;
    }
  } else {
    // Exponential notation
    *p++ = dbuf[0];
    if (nDigits > 1) {
      *p++ = '.';
      memcpy(p, dbuf + 1, nDigits - 1);
      p += nDigits - 1;
    }
    *p++ = 'e';
    int ae = e10 < 0 ? -e10 : e10;
    *p++ = e10 < 0 ? '-' : '+';
    if (ae >= 100) {
      *p++ = '0' + ae / 100;
      ae %= 100;
    }
    *p++ = '0' + ae / 10;
    *p++ = '0' + ae % 10;
  }

  out.append(buf, p - buf);
}
