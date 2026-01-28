#pragma once

#include "endiantools.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <limits>
#include <cfloat>
#include <cmath>

#include <assert.h>

#ifdef __GNUC__
#if defined(__x86_64__) || defined(__i386__)
#include "x86intrin.h"
#endif
#endif

#ifdef _MSC_VER
#include "intrin.h"
#endif

#include <string>
#include <type_traits>

static_assert(sizeof(unsigned int) == sizeof(uint32_t));
static_assert(sizeof(unsigned long long) == sizeof(uint64_t));
static_assert(sizeof(unsigned int) == 4 || sizeof(unsigned int) == 8);
static_assert(sizeof(unsigned long) == 4 || sizeof(unsigned long) == 8);

static inline void addc64(uint64_t *a, uint64_t b, uint64_t *carryFlag)
{
#ifdef __clang__
  *a = __builtin_addcll(*a, b, *carryFlag, carryFlag);
#elif __GNUC__
  // use intrinsic for x86
#if defined(__x86_64__) || defined(__i386__)
  *carryFlag = _addcarry_u64(*carryFlag, *a, b, reinterpret_cast<unsigned long long*>(a));
#else
  unsigned __int128 r = static_cast<unsigned __int128>(*a) + b + *carryFlag;
  *a = r;
  *carryFlag = r >> 64;
#endif
#elif _MSC_VER
  *carryFlag = _addcarry_u64(*carryFlag, *a, b, a);
#endif
}

static inline void subb64(uint64_t *a, uint64_t b, uint64_t *borrowFlag)
{
#ifdef __clang__
  *a = __builtin_subcll(*a, b, *borrowFlag, borrowFlag);
#elif __GNUC__
  // use intrinsic for x86
#if defined(__x86_64__) || defined(__i386__)
  *borrowFlag = _subborrow_u64(*borrowFlag, *a, b, reinterpret_cast<unsigned long long*>(a));
#else
  unsigned __int128 r = static_cast<unsigned __int128>(*a) - b - *borrowFlag;
  *a = r;
  *borrowFlag = -(r >> 64);
#endif
#elif _MSC_VER
  *borrowFlag = _subborrow_u64(*borrowFlag, *a, b, a);
#endif
}

static inline void mulx64(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi)
{
#ifndef _MSC_VER
  unsigned __int128 result = static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
  *lo = result;
  *hi = result >> 64;
#else
  *lo = a * b;
  *hi = __umulh(a, b);
#endif
}

// Division
static inline void divmod64(uint64_t lo, uint64_t hi, uint64_t divisor, uint64_t *result, uint64_t *remainder)
{
#ifndef _MSC_VER
  *result = ((static_cast<unsigned __int128>(hi) << 64) + lo) / divisor;
  *remainder = ((static_cast<unsigned __int128>(hi) << 64) + lo) % divisor;
#else
  *result = _udiv128(hi, lo, divisor, remainder);
#endif
}

static inline unsigned clz64(uint64_t x)
{
  assert(x != 0);
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<unsigned>(__builtin_clzll(x));
#elif defined(_MSC_VER)
  unsigned long idx;
#if defined(_M_X64) || defined(_M_ARM64)
  _BitScanReverse64(&idx, x);
  return 63u - static_cast<unsigned>(idx);
#else
  uint32_t hi = static_cast<uint32_t>(x >> 32);
  if (hi) {
    _BitScanReverse(&idx, hi);
    return 31u - static_cast<unsigned>(idx);
  } else {
    uint32_t lo = static_cast<uint32_t>(x);
    _BitScanReverse(&idx, lo);
    return 63u - static_cast<unsigned>(idx);
  }
#endif
#else
  unsigned n = 0;
  while ((x & (1ULL << 63)) == 0) { x <<= 1; ++n; }
  return n;
#endif
}

template<unsigned Bits>
class UInt {
public:
  UInt() {
    memset(Data_, 0, sizeof(Data_));
  }

  // Allow construction from unsigned integral types only
  template<typename T, std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, int> = 0>
  UInt(T n) {
    Data_[0] = static_cast<uint64_t>(n);
    for (size_t i = 1; i < LimbsNum; i++)
      Data_[i] = 0;
  }

  // Prevent implicit conversion from signed integers and floating point
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  UInt(T) = delete;

  // Allow implicit construction only from smaller or equal size UInt
  template<unsigned OtherBits, std::enable_if_t<(OtherBits <= Bits), int> = 0>
  UInt(const UInt<OtherBits> &other) {
    constexpr size_t copyLimbs = UInt<OtherBits>::LimbsNum;
    memcpy(Data_, other.data(), copyLimbs * sizeof(uint64_t));
    if constexpr (LimbsNum > copyLimbs)
      memset(Data_ + copyLimbs, 0, (LimbsNum - copyLimbs) * sizeof(uint64_t));
  }

  // static constructors
  static UInt<Bits> zero() {
    UInt<Bits> result;
    memset(result.Data_, 0, sizeof(result.Data_));
    return result;
  }

  static UInt<Bits> max() {
    UInt<Bits> result;
    memset(result.Data_, 0xFF, sizeof(result.Data_));
    return result;
  }

  static UInt<Bits> fromHex(const char *hex) {
    UInt<Bits> result;
    result.setHex(hex);
    return result;
  }

  // Truncate from larger UInt (explicit conversion)
  template<unsigned OtherBits, std::enable_if_t<(OtherBits > Bits), int> = 0>
  static UInt<Bits> truncate(const UInt<OtherBits> &other) {
    UInt<Bits> result;
    memcpy(result.Data_, other.data(), LimbsNum * sizeof(uint64_t));
    return result;
  }

  static UInt<Bits> fromDouble(double d) {
    static double multiplier = std::pow(FLT_RADIX, DBL_MANT_DIG);

    int exponent = 0;
    uint64_t mantissa = static_cast<uint64_t>(std::frexp(d, &exponent) * multiplier);
    exponent -= DBL_MANT_DIG;

    UInt<Bits> result;
    result.Data_[0] = mantissa;
    for (size_t i = 1; i < LimbsNum; i++)
      result.Data_[i] = 0;

    if (exponent < 0)
      result.shr(-exponent);
    else
      result.shl(exponent);

    return result;
  }

  const uint64_t *data() const { return Data_; }
  uint64_t *data() { return Data_; }
  const uint8_t *rawData() const { return reinterpret_cast<const uint8_t*>(Data_); }
  uint8_t *rawData() { return reinterpret_cast<uint8_t*>(Data_); }
  size_t rawSize() const { return sizeof(Data_); }

  void exportLE(uint8_t *out) {
    if (isLittleEndian()) {
      memcpy(out, Data_, sizeof(Data_));
    } else {
      uint64_t *out64 = reinterpret_cast<uint64_t*>(out);
      for (unsigned i = 0; i < LimbsNum; i++)
        out64[i] = writele(Data_[i]);
    }
  }

  // Conversion functions
  uint64_t low64() const { return Data_[0]; }

  double getDouble() const {
    double ret = 0.0;
    double fact = 1.0;
    for (size_t i = 0; i < LimbsNum; i++) {
      ret += fact * Data_[i];
      fact *= 18446744073709551616.0; // 2^64
    }
    return ret;
  }

  // Divide two UInts and return result as double with minimal precision loss
  template<unsigned BitsA, unsigned BitsB>
  static double fpdiv(const UInt<BitsA> &dividend, const UInt<BitsB> &divisor) {
    if (divisor.isZero() || dividend.isZero())
      return 0.0;

    unsigned bitsA = dividend.bits();
    unsigned bitsB = divisor.bits();

    // Normalize both to fit in 64 bits for maximum precision
    UInt<BitsA> normA = dividend;
    UInt<BitsB> normB = divisor;
    int shiftA = 0;
    int shiftB = 0;

    if (bitsA > 64) {
      shiftA = bitsA - 64;
      normA >>= shiftA;
    }
    if (bitsB > 64) {
      shiftB = bitsB - 64;
      normB >>= shiftB;
    }

    double result = static_cast<double>(normA.low64()) / static_cast<double>(normB.low64());

    // Adjust for shifts: dividend was shifted right by shiftA, divisor by shiftB
    // So result needs to be multiplied by 2^(shiftA - shiftB)
    int totalShift = shiftA - shiftB;
    return std::ldexp(result, totalShift);
  }

  unsigned bits() const {
    for (size_t i = LimbsNum; i > 0; --i) {
      if (Data_[i - 1] != 0)
        return static_cast<unsigned>((i - 1) * 64 + (64 - clz64(Data_[i - 1])));
    }
    return 0;
  }

  void getHex(char *out, bool leadingZeroes = true, bool zeroxPrefix = false, bool upperCase = false) const {
    if (zeroxPrefix) {
      *out++ = '0';
      *out++ = 'x';
    }

    char *p = out;

    int bitOffset = LimbSize;
    int limbOffset = LimbsNum-1;
    uint64_t limb = Data_[limbOffset];
    bool stillZero = true;
    for (;;) {
      bitOffset -= 4;
      if (bitOffset < 0) {
        if (--limbOffset < 0)
          break;
        bitOffset = LimbSize - 4;
        limb = Data_[limbOffset];
      }

      uint8_t v = (limb >> bitOffset) & 0xF;
      if (v != 0)
        stillZero = false;

      if (!leadingZeroes && stillZero)
        continue;

      *p++ = encodeHex(v, upperCase);
    }

    if (p == out)
      *p++ = '0';
    *p = 0;
  }

  std::string getHex(bool leadingZeroes = true, bool zeroxPrefix = false, bool upperCase = false) const {
    char data[Bits/4 + 8];
    getHex(data, leadingZeroes, zeroxPrefix, upperCase);
    return data;
  }

  // Decimal conversion
  std::string getDecimal() const {
    // Estimate decimal digits from bit count using: digits ≈ bits * log10(2)
    // Integer approximation: 77/256 ≈ 0.30078 ≈ log10(2)
    // For zero: bits()=0, minDigits=1, one iteration produces "0"
    unsigned numBits = bits();
    size_t maxDigits = (static_cast<size_t>(numBits) * 77) / 256 + 2;
    size_t minDigits = numBits > 3 ? (static_cast<size_t>(numBits - 1) * 77) / 256 + 1 : 1;

    char buffer[Bits / 3 + 2];
    UInt<Bits> tmp = *this;
    size_t pos = maxDigits;

    // Extract minDigits without checking isZero (guaranteed to have at least this many)
    for (size_t i = 0; i < minDigits; i++) {
      buffer[--pos] = '0' + static_cast<char>(tmp.divmod64(10u));
    }

    // Extract remaining 0-2 digits
    while (tmp.nonZero()) {
      buffer[--pos] = '0' + static_cast<char>(tmp.divmod64(10u));
    }

    return std::string(buffer + pos, maxDigits - pos);
  }

  void setHex(const char *hex) {
    if (hex[0] == '0' && hex[1] == 'x')
      hex += 2;

    // Search end of hex string
    const char *p = hex;
    while (isHexDigit(*p))
      p++;
    p--;

    memset(Data_, 0, sizeof(Data_));

    uint64_t limb = 0;
    unsigned bitOffset = 0;
    unsigned limbOffset = 0;
    while (p >= hex) {
      limb |= static_cast<uint64_t>(decodeHex(*p)) << bitOffset;
      p--;
      bitOffset += 4;
      if (bitOffset >= LimbSize) {
        Data_[limbOffset++] = limb;
        if (limbOffset >= LimbsNum)
          break;

        limb = 0;
        bitOffset = 0;
      }
    }

    if (limbOffset < LimbsNum)
      Data_[limbOffset] = limb;
  }

  // Compare functions
  bool isZero() const {
    for (size_t i = 0; i < LimbsNum; i++) {
      if (Data_[i] != 0)
        return false;
    }
    return true;
  }

  bool nonZero() const {
    for (size_t i = 0; i < LimbsNum; i++) {
      if (Data_[i] != 0)
        return true;
    }
    return false;
  }

  // UInt is unsigned, but this function can be used to detect
  // underflow after subtraction (MSB set means wrapped around)
  bool isNegative() const {
    return (Data_[LimbsNum - 1] >> 63) != 0;
  }

  int cmp64(uint64_t n) const {
    for (size_t i = LimbsNum; i > 1; --i) {
      if (Data_[i - 1] != 0)
        return 1;
    }
    if (Data_[0] < n) return -1;
    if (Data_[0] > n) return 1;
    return 0;
  }

  template<unsigned OperandBits>
  int cmp(const UInt<OperandBits> &n) const { return cmp(Data_, LimbsNum, n.Data_, n.LimbsNum); }

  // Addiction functions
  void add64(uint64_t n) {
    uint64_t carryFlag = 0;
    addc64(reinterpret_cast<uint64_t*>(&Data_[0]), n, &carryFlag);
    for (size_t i = 1; i < LimbsNum; i++)
      addc64(reinterpret_cast<uint64_t*>(&Data_[i]), 0, &carryFlag);
  }

  template<unsigned OperandBits>
  void add(const UInt<OperandBits> &n) {
    uint64_t carry = 0;
    size_t minSize = std::min(LimbsNum, n.LimbsNum);
    for (size_t i = 0; i < minSize; i++)
      addc64(&Data_[i], n.Data_[i], &carry);
    for (size_t i = minSize; i < LimbsNum; i++)
      addc64(&Data_[i], 0, &carry);
  }

  // Substraction functions
  void sub64(uint64_t n) {
    uint64_t borrowFlag = 0;
    subb64(reinterpret_cast<uint64_t*>(&Data_[0]), n, &borrowFlag);
    for (size_t i = 1; i < LimbsNum; i++)
      subb64(reinterpret_cast<uint64_t*>(&Data_[i]), 0, &borrowFlag);
  }

  template<unsigned OperandBits>
  void sub(const UInt<OperandBits> &n) {
    uint64_t borrow = 0;
    size_t minSize = std::min(LimbsNum, n.LimbsNum);
    for (size_t i = 0; i < minSize; i++)
      subb64(&Data_[i], n.Data_[i], &borrow);
    for (size_t i = minSize; i < LimbsNum; i++)
      subb64(&Data_[i], 0, &borrow);
  }

  // Shift functions
  void shl(unsigned shift) { shl(Data_, LimbsNum, shift); }
  void shr(unsigned shift) { shr(Data_, LimbsNum, shift); }

  // Multiplication
  void mul64(uint64_t n) {
    uint64_t oldHi = 0;
    uint64_t carry = 0;
    for (size_t i = 0; i < LimbsNum; i++) {
      uint64_t hi;
      mulx64(Data_[i], n, reinterpret_cast<uint64_t*>(&Data_[i]), &hi);
      addc64(reinterpret_cast<uint64_t*>(&Data_[i]), oldHi, &carry);
      oldHi = hi;
    }
  }

  // Multiply by double with minimal precision loss
  // Extracts 53-bit mantissa, multiplies as integer, then shifts by exponent
  void mulfp(double d) {
    static const double multiplier = std::pow(FLT_RADIX, DBL_MANT_DIG);

    int exponent = 0;
    uint64_t mantissa = static_cast<uint64_t>(std::frexp(d, &exponent) * multiplier);
    exponent -= DBL_MANT_DIG;

    if (Data_[LimbsNum - 1] == 0) {
      // No overflow risk, multiply in place
      mul64(mantissa);

      if (exponent < 0)
        shr(-exponent);
      else if (exponent > 0)
        shl(exponent);
    } else {
      // Use extended precision to avoid overflow
      UInt<Bits + 64> tmp(*this);
      tmp.mul64(mantissa);

      if (exponent < 0)
        tmp.shr(-exponent);
      else if (exponent > 0)
        tmp.shl(exponent);

      *this = truncate(tmp);
    }
  }

  template<unsigned OperandBits>
  void mul(const UInt<OperandBits> &n) {
    // Use product scan
    UInt<Bits> result;
    uint64_t accLow = 0;
    uint64_t accHi = 0;
    uint64_t carry = 0;
    for (size_t i = 0; i < LimbsNum; i++) {
      int off1 = std::max(static_cast<int>(i) - static_cast<int>(n.LimbsNum) + 1, 0);
      int off2 = i - off1;
      int count = std::min(static_cast<int>(LimbsNum)-off1, static_cast<int>(i)-off1+1);
      for (int j = 0; j < count; j++, off1++, off2--) {
        uint64_t lo;
        uint64_t hi;
        uint64_t lcarry = 0;
        mulx64(Data_[off1], n.Data_[off2], &lo, &hi);
        addc64(&accLow, lo, &lcarry);
        addc64(&accHi, hi, &lcarry);
        addc64(&carry, 0, &lcarry);
      }

      result.Data_[i] = accLow;
      accLow = accHi;
      accHi = carry;
      carry = 0;
    }

    memcpy(Data_, result.Data_, sizeof(uint64_t)*LimbsNum);
  }

  // Division by 64-bit value
  // Non-const version: stores quotient in *this, returns remainder
  uint64_t divmod64(uint64_t n) {
    uint64_t rem = 0;
    for (size_t i = LimbsNum; i > 0; --i)
      ::divmod64(Data_[i - 1], rem, n, &Data_[i - 1], &rem);
    return rem;
  }

  // Const version: stores quotient in *quotient (if not null), returns remainder
  uint64_t divmod64(uint64_t n, UInt<Bits> *quotient = nullptr) const {
    uint64_t rem = 0;
    uint64_t q;
    for (size_t i = LimbsNum; i > 0; --i)
      ::divmod64(Data_[i - 1], rem, n, quotient ? &quotient->Data_[i - 1] : &q, &rem);
    return rem;
  }

  template<unsigned OperandBits>
  void divmod(const UInt<OperandBits> &divisor, UInt<Bits> *quotient, UInt<Bits> *remainder) const {
    size_t divisorLimbs = 0;
    size_t dividendLimbs = 0;

    for (size_t i = std::min(LimbsNum, UInt<OperandBits>::LimbsNum); i > 0; --i) {
      if (divisor.Data_[i - 1] != 0) {
        divisorLimbs = i;
        break;
      }
    }

    for (size_t i = LimbsNum; i > 0; --i) {
      if (Data_[i - 1] != 0) {
        dividendLimbs = i;
        break;
      }
    }

    if (divisorLimbs == 0) {
      // division by zero, cause OS exception
      uint64_t x = Data_[0];
      x /= divisor.Data_[0];
      return;
    }

    if (dividendLimbs == 0) {
      // dividend is zero, so quotient and remainder also 0
      if (quotient)
        *quotient  = UInt<Bits>::zero();
      if (remainder)
        *remainder = UInt<Bits>::zero();
      return;
    }

    // Quick exit if U < V
    if (dividendLimbs < divisorLimbs) {
      if (quotient)
        *quotient  = UInt<Bits>::zero();
      if (remainder)
        *remainder = *this;
      return;
    }

    // -------- Knuth D (base 2^64) --------
    const size_t m = dividendLimbs - divisorLimbs;

    uint64_t u[LimbsNum + 1] = {}; // normalized dividend + extra limb
    uint64_t v[LimbsNum]     = {}; // normalized divisor
    uint64_t qd[LimbsNum]    = {}; // quotient digits

    memcpy(v, divisor.Data_, divisorLimbs * sizeof(uint64_t));
    memcpy(u, Data_, dividendLimbs * sizeof(uint64_t));
    u[dividendLimbs] = 0;

    // Normalization: shift so that MSB of v[n-1] becomes 1
    const unsigned s = clz64(v[divisorLimbs - 1]);
    if (s) {
      shl(v, divisorLimbs, s);
      shl(u, dividendLimbs + 1, s);
    }

    // Main loop over quotient digits: j = m..0
    for (size_t jj = m + 1; jj-- > 0; ) {
      const size_t j = jj;

      uint64_t qhat = 0;
      uint64_t rhat = 0;
      bool rhat_ge_B = false;

      const uint64_t ujn  = u[j + divisorLimbs];
      const uint64_t ujn1 = u[j + divisorLimbs - 1];

      // Estimate qhat from two most significant words
      // (protection against case ujn == v[n-1], where true division would give 2^64)
      if (ujn >= v[divisorLimbs - 1]) {
        qhat = std::numeric_limits<uint64_t>::max(); // B-1
        rhat = ujn1;
        uint64_t c = 0;
        addc64(&rhat, v[divisorLimbs - 1], &c);
        rhat_ge_B = (c != 0); // rhat >= B => correction check not needed
      } else {
        ::divmod64(ujn1, ujn, v[divisorLimbs - 1], &qhat, &rhat);
      }

      // Correction of qhat (Knuth D3)
      if (divisorLimbs >= 2 && !rhat_ge_B) {
        const uint64_t ujn2 = u[j + divisorLimbs - 2];
        for (;;) {
          uint64_t p_lo, p_hi;
          mulx64(qhat, v[divisorLimbs - 2], &p_lo, &p_hi);

          // if qhat*v[n-2] <= rhat*B + ujn2, then ok
          if (p_hi < rhat)
            break;
          if (p_hi == rhat && p_lo <= ujn2)
            break;

          qhat--;

          uint64_t c = 0;
          addc64(&rhat, v[divisorLimbs - 1], &c);
          if (c) {
            // rhat became >= B — further correction definitely not required
            rhat_ge_B = true;
            break;
          }
        }
      }

      // D4: u[j..j+n] -= qhat * v[0..n-1]
      uint64_t borrow = 0;
      uint64_t carry  = 0;
      for (size_t i = 0; i < divisorLimbs; ++i) {
        uint64_t p_lo, p_hi;
        mulx64(qhat, v[i], &p_lo, &p_hi);

        uint64_t c = 0;
        addc64(&p_lo, carry, &c);
        p_hi += c;

        uint64_t b = borrow;
        subb64(&u[j + i], p_lo, &b);
        borrow = b;

        carry = p_hi;
      }

      {
        uint64_t b = borrow;
        subb64(&u[j + divisorLimbs], carry, &b);
        borrow = b;
      }

      // D6: if went negative — rollback (add divisor back) and decrease qhat
      if (borrow) {
        qhat--;

        uint64_t c = 0;
        for (size_t i = 0; i < divisorLimbs; ++i)
          addc64(&u[j + i], v[i], &c);
        addc64(&u[j + divisorLimbs], 0, &c);
      }

      qd[j] = qhat;
    }

    // Denormalization of remainder: r = u[0..n-1] >> s
    UInt<Bits> qOut;
    UInt<Bits> rOut;

    for (size_t i = 0; i < LimbsNum; ++i)
      qOut.Data_[i] = (i <= m) ? qd[i] : 0;

    for (size_t i = 0; i < divisorLimbs; ++i)
      rOut.Data_[i] = u[i];
    for (size_t i = divisorLimbs; i < LimbsNum; ++i)
      rOut.Data_[i] = 0;

    if (s)
      shr(rOut.Data_, divisorLimbs, s);

    if (quotient)
      *quotient  = qOut;
    if (remainder)
      *remainder = rOut;
  }

  // operator overload
  template<unsigned OperandBits> friend class UInt;

  // comparision
  bool operator==(uint64_t n) const { return cmp64(n) == 0; }
  bool operator!=(uint64_t n) const { return cmp64(n) != 0; }
  bool operator<(uint64_t n) const { return cmp64(n) < 0; }
  bool operator<=(uint64_t n) const { return cmp64(n) <= 0; }
  bool operator>(uint64_t n) const { return cmp64(n) > 0; }
  bool operator>=(uint64_t n) const { return cmp64(n) >= 0; }

  template<unsigned OperandBits>
    bool operator==(const UInt<OperandBits> &n) const { return cmp(n) == 0; }
  template<unsigned OperandBits>
    bool operator!=(const UInt<OperandBits> &n) const { return cmp(n) != 0; }
  template<unsigned OperandBits>
    bool operator<(const UInt<OperandBits> &n) const { return cmp(n) < 0; }
  template<unsigned OperandBits>
    bool operator<=(const UInt<OperandBits> &n) const { return cmp(n) <= 0; }
  template<unsigned OperandBits>
    bool operator>(const UInt<OperandBits> &n) const { return cmp(n) > 0; }
  template<unsigned OperandBits>
    bool operator>=(const UInt<OperandBits> &n) const { return cmp(n) >= 0; }

  // addition
  UInt<Bits> operator+=(uint64_t n) { add64(n); return *this; }
  template<unsigned OperandBits>
    UInt<Bits> operator+=(const UInt<OperandBits> &n) { add(n); return *this; }
  template<typename T, std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, int> = 0>
    UInt<Bits> friend operator+(const UInt<Bits> &a, T b) { return UInt<Bits>(a) += b; }
  template<unsigned OperandBits>
    friend UInt<Bits> operator+(const UInt<Bits> &a, const UInt<OperandBits> &b) { return UInt<Bits>(a) += b; }

  // substraction
  UInt<Bits> operator-=(uint64_t n) { sub64(n); return *this; }
  template<unsigned OperandBits>
    UInt<Bits> operator-=(const UInt<OperandBits> &n) { sub(n); return *this; }
  template<typename T, std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, int> = 0>
    UInt<Bits> friend operator-(const UInt<Bits> &a, T b) { return UInt<Bits>(a) -= b; }
  template<unsigned OperandBits>
    friend UInt<Bits> operator-(const UInt<Bits> &a, const UInt<OperandBits> &b) { return UInt<Bits>(a) -= b; }

  // shift
  UInt<Bits> operator<<=(unsigned n) { shl(n); return *this; }
  UInt<Bits> operator>>=(unsigned n) { shr(n); return *this; }
  UInt<Bits> friend operator<<(const UInt<Bits> &a, unsigned n) { return UInt<Bits>(a) <<= n; }
  UInt<Bits> friend operator>>(const UInt<Bits> &a, unsigned n) { return UInt<Bits>(a) >>= n; }

  // multiplication short
  UInt<Bits> operator*=(uint64_t n) { mul64(n); return *this; }
  template<typename T, std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, int> = 0>
    UInt<Bits> friend operator*(const UInt<Bits> &a, T b) { return UInt<Bits>(a) *= b; }

  // multiplication long
  template<unsigned OperandBits>
    UInt<Bits> operator*=(const UInt<OperandBits> &n) { mul(n); return *this; }
  template<unsigned OperandBits>
    friend UInt<Bits> operator*(const UInt<Bits> &a, const UInt<OperandBits> &b) { return UInt<Bits>(a) *= b; }

  // division short
  UInt<Bits> operator/=(uint64_t n) { divmod64(n); return *this; }
  template<typename T, std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, int> = 0>
    UInt<Bits> friend operator/(const UInt<Bits> &a, T b) { return UInt<Bits>(a) /= b; }

  // modulo short
  uint64_t operator%(uint64_t n) const { return divmod64(n); }


  // division long
  template<unsigned OperandBits>
  UInt<Bits> operator/=(const UInt<OperandBits> &n) {
    divmod(n, this, nullptr);
    return *this;
  }
  template<unsigned OperandBits>
  friend UInt<Bits> operator/(const UInt<Bits> &a, const UInt<OperandBits> &b) {
    UInt<Bits> quotient;
    a.divmod(b, &quotient, nullptr);
    return quotient;
  }

  // modulo long
  template<unsigned OperandBits>
  UInt<Bits> operator%=(const UInt<OperandBits> &n) {
    divmod(n, nullptr, this);
    return *this;
  }
  template<unsigned OperandBits>
  friend UInt<Bits> operator%(const UInt<Bits> &a, const UInt<OperandBits> &b) {
    UInt<Bits> remainder;
    a.divmod(b, nullptr, &remainder);
    return remainder;
  }

  // bitwise not
  void not_() {
    for (size_t i = 0; i < LimbsNum; ++i)
      Data_[i] = ~Data_[i];
  }

  UInt<Bits> operator~() const {
    UInt<Bits> result(*this);
    result.not_();
    return result;
  }

  // Two's complement negation (in-place)
  void negate() {
    not_();
    add64(1u);
  }

  // Unary minus
  UInt<Bits> operator-() const {
    UInt<Bits> result(*this);
    result.negate();
    return result;
  }

  // Prevent operations with signed integers and floating point types
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  bool operator==(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  bool operator!=(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  bool operator<(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  bool operator<=(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  bool operator>(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  bool operator>=(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  UInt<Bits> operator+=(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  UInt<Bits> operator-=(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  UInt<Bits> operator*=(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  UInt<Bits> operator/=(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  uint64_t operator%(T) const = delete;

  // Prevent member functions with signed/floating point arguments
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  int cmp64(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  void add64(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  void sub64(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  void mul64(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  uint64_t divmod64(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  uint64_t divmod64(T, UInt<Bits>*) const = delete;

private:
  static constexpr size_t LimbSize = 8 * sizeof(uint64_t);
  static constexpr size_t LimbsNum = Bits / 8 / sizeof(uint64_t);
  static_assert(LimbsNum >= 2);

private:
  static bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
  }

  static uint8_t decodeHex(char c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    else if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    else
      return c - 'a' + 10;
  }

  static char encodeHex(uint8_t value, bool upperCase) {
    return value < 10 ? '0'+value : (upperCase ? 'A'+value-10 : 'a'+value-10);
  }

  // Compare two arrays of limbs, returns -1 if left < right, 0 if equal, 1 if left > right
  static int cmp(const uint64_t *left, size_t leftLen, const uint64_t *right, size_t rightLen) {
    // Check extra limbs of the longer operand
    if (leftLen > rightLen) {
      for (size_t i = leftLen; i > rightLen; --i) {
        if (left[i - 1] != 0)
          return 1;
      }
    } else if (rightLen > leftLen) {
      for (size_t i = rightLen; i > leftLen; --i) {
        if (right[i - 1] != 0)
          return -1;
      }
    }

    // Compare common limbs from most significant to least significant
    size_t minLen = std::min(leftLen, rightLen);
    for (size_t i = minLen; i > 0; --i) {
      if (left[i - 1] < right[i - 1])
        return -1;
      if (left[i - 1] > right[i - 1])
        return 1;
    }

    return 0;
  }

  // Shift array of limbs left by arbitrary number of bits
  static void shl(uint64_t *data, size_t len, unsigned shift) {
    if (shift == 0 || len == 0)
      return;

    const size_t limbShift = shift / 64;
    const unsigned bitShift = shift % 64;

    if (limbShift >= len) {
      memset(data, 0, len * sizeof(uint64_t));
      return;
    }

    const size_t remaining = len - limbShift;

    if (bitShift == 0) {
      // Shift by whole limbs only
      memmove(data + limbShift, data, remaining * sizeof(uint64_t));
    } else {
      // Shift by limbs + bits
      const unsigned invBitShift = 64 - bitShift;
      for (size_t i = len; i-- > limbShift + 1; )
        data[i] = (data[i - limbShift] << bitShift) | (data[i - limbShift - 1] >> invBitShift);
      data[limbShift] = data[0] << bitShift;
    }

    // Zero out lower limbs
    memset(data, 0, limbShift * sizeof(uint64_t));
  }

  // Shift array of limbs right by arbitrary number of bits
  static void shr(uint64_t *data, size_t len, unsigned shift) {
    if (shift == 0 || len == 0)
      return;

    const size_t limbShift = shift / 64;
    const unsigned bitShift = shift % 64;

    if (limbShift >= len) {
      memset(data, 0, len * sizeof(uint64_t));
      return;
    }

    const size_t remaining = len - limbShift;

    if (bitShift == 0) {
      // Shift by whole limbs only
      memmove(data, data + limbShift, remaining * sizeof(uint64_t));
    } else {
      // Shift by limbs + bits
      const unsigned invBitShift = 64 - bitShift;
      for (size_t i = 0; i < remaining - 1; i++)
        data[i] = (data[i + limbShift] >> bitShift) | (data[i + limbShift + 1] << invBitShift);
      data[remaining - 1] = data[len - 1] >> bitShift;
    }

    // Zero out upper limbs
    memset(data + remaining, 0, limbShift * sizeof(uint64_t));
  }

private:
  uint64_t Data_[LimbsNum];
};

// Compact format functions for UInt<256> (Bitcoin difficulty target encoding)
static inline UInt<256> uint256Compact(uint32_t nCompact, bool *pfNegative = nullptr, bool *pfOverflow = nullptr)
{
  UInt<256> result;
  int nSize = nCompact >> 24;
  uint32_t nWord = nCompact & 0x007fffff;

  if (nSize <= 3) {
    nWord >>= 8 * (3 - nSize);
    result = nWord;
  } else {
    result = nWord;
    result <<= 8 * (nSize - 3);
  }

  if (pfNegative)
    *pfNegative = nWord != 0 && (nCompact & 0x00800000) != 0;
  if (pfOverflow)
    *pfOverflow = nWord != 0 && ((nSize > 34) ||
                                 (nWord > 0xff && nSize > 33) ||
                                 (nWord > 0xffff && nSize > 32));
  return result;
}

static inline uint32_t uint256GetCompact(const UInt<256> &value, bool fNegative = false)
{
  int nSize = (value.bits() + 7) / 8;
  uint32_t nCompact = 0;

  if (nSize <= 3) {
    nCompact = static_cast<uint32_t>(value.low64() << 8 * (3 - nSize));
  } else {
    UInt<256> bn = value >> 8 * (nSize - 3);
    nCompact = static_cast<uint32_t>(bn.low64());
  }

  // The 0x00800000 bit denotes the sign.
  // Thus, if it is already set, divide the mantissa by 256 and increase the exponent.
  if (nCompact & 0x00800000) {
    nCompact >>= 8;
    nSize++;
  }

  assert((nCompact & ~0x007fffffU) == 0);
  assert(nSize < 256);
  nCompact |= nSize << 24;
  nCompact |= (fNegative && (nCompact & 0x007fffff) ? 0x00800000 : 0);
  return nCompact;
}
