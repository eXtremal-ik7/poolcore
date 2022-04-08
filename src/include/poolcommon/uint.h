#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>

#include <assert.h>

#ifdef __GNUC__
#if defined(__x86_64__) || defined(__i386__)
#include "x86intrin.h"
#endif
#endif

#ifdef _MSC_VER
#include "intrin.h"
#endif

using LimbTy = uint64_t;

static_assert(sizeof(unsigned int) == sizeof(uint32_t));
static_assert(sizeof(unsigned long long) == sizeof(uint64_t));
static_assert(sizeof(unsigned int) == 4 || sizeof(unsigned int) == 8);
static_assert(sizeof(unsigned long) == 4 || sizeof(unsigned long) == 8);

// Addition with carry
static inline void addc32(uint32_t *a, uint32_t b, uint32_t *carryFlag)
{
#ifdef __clang__
  *a = __builtin_addc(*a, b, *carryFlag, carryFlag);
#elif __GNUC__
  // use intrinsic for x86
#if defined(__x86_64__) || defined(__i386__)
  *carryFlag = _addcarry_u32(*carryFlag, *a, b, a);
#else
  uint64_t r = static_cast<uint64_t>(*a) + b + *carryFlag;
  *a = r;
  *carryFlag = r >> 32;
#endif
#elif _MSC_VER
  *carryFlag = _addcarry_u32(*carryFlag, *a, b, a);
#endif
}

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

// Substraction with borrow
static inline void subb32(uint32_t *a, uint32_t b, uint32_t *borrowFlag)
{
#ifdef __clang__
  *a = __builtin_subc(*a, b, *borrowFlag, borrowFlag);
#elif __GNUC__
  // use intrinsic for x86
#if defined(__x86_64__) || defined(__i386__)
  *borrowFlag = _subborrow_u32(*borrowFlag, *a, b, a);
#else
  uint64_t r = static_cast<uint64_t>(*a) - b - *borrowFlag;
  *a = r;
  *borrowFlag = -(r >> 32);
#endif
#elif _MSC_VER
  *borrowFlag = _subborrow_u32(*borrowFlag, *a, b, a);
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

// Multiplication
static inline void mulx32(uint32_t a, uint32_t b, uint32_t *lo, uint32_t *hi)
{
  uint64_t result = static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
  *lo = result;
  *hi = result >> 32;
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
static inline void divmod32(uint32_t lo, uint32_t hi, uint32_t divisor, uint32_t *result, uint32_t *remainder)
{
  *result = ((static_cast<uint64_t>(hi) << 32) + lo) / divisor;
  *remainder = ((static_cast<uint64_t>(hi) << 32) + lo) % divisor;
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

template<unsigned Bits>
class UInt {
public:
  UInt() {
    for (size_t i = 0; i < LimbsNum; i++)
      Data_[i] = 0;
  }

  UInt(uint32_t n) {
    Data_[0] = n;
    for (size_t i = 1; i < LimbsNum; i++)
      Data_[i] = 0;
  }

  UInt(uint64_t n) {
    if (is64()) {
      Data_[0] = n;
      for (size_t i = 1; i < LimbsNum; i++)
        Data_[i] = 0;
    } else {
      Data_[0] = n;
      Data_[1] = n >> 32;
      for (size_t i = 2; i < LimbsNum; i++)
        Data_[i] = 0;
    }
  }

  // static constructors
  static UInt<Bits> fromHex(const char *hex) {
    UInt<Bits> result;

    // Search end of hex string
    const char *p = hex;
    while (isHexDigit(*p))
      p++;
    p--;

    LimbTy limb = 0;
    unsigned bitOffset = 0;
    unsigned limbOffset = 0;
    while (p >= hex) {
      limb |= static_cast<LimbTy>(decodeHex(*p)) << bitOffset;
      p--;
      bitOffset += 4;
      if (bitOffset >= LimbSize) {
        result.Data_[limbOffset++] = limb;
        if (limbOffset >= LimbsNum)
          break;

        limb = 0;
        bitOffset = 0;
      }
    }

    if (limbOffset < LimbsNum)
      result.Data_[limbOffset] = limb;
    return result;
  }

  // Conversion functions
  uint32_t low32() const {
    return Data_[0];
  }

  uint64_t low64() const {
    return is64() ?
      Data_[0] :
      (static_cast<uint64_t>(Data_[1]) << 32) | Data_[0];
  }

  void toHex(char *out, bool leadingZeroes = true, bool zeroxPrefix = false, bool upperCase = false) const {
    if (zeroxPrefix) {
      *out++ = '0';
      *out++ = 'x';
    }

    char *p = out;

    int bitOffset = LimbSize;
    int limbOffset = LimbsNum-1;
    LimbTy limb = Data_[limbOffset];
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

  // Compare functions
  bool cmpeq32(uint32_t n) {
    return is64() ? cmpeq64(n) : isSingleLimb() && Data_[0] == n;
  }

  bool cmpeq64(uint64_t n) {
    if (is64()) {
      return isSingleLimb() && Data_[0] == n;
    } else {
      return isDoubleLimb() && low64() == n;
    }
  }

  template<unsigned OperandBits>
  bool cmpeq(const UInt<OperandBits> &n) {
    const LimbTy *shorterData;
    const LimbTy *longerData;
    size_t shorterSize;
    size_t longerSize;
    if (LimbsNum <= n.LimbsNum) {
      shorterData = Data_;
      longerData = n.Data_;
      shorterSize = LimbsNum;
      longerSize = n.LimbsNum;
    } else {
      shorterData = n.Data_;
      longerData = Data_;
      shorterSize = n.LimbsNum;
      longerSize = LimbsNum;
    }

    for (size_t i = 0; i < shorterSize; i++) {
      if (shorterData[i] != longerData[i])
        return false;
    }

    for (size_t i = shorterSize; i < longerSize; i++) {
      if (longerData[i] != 0)
        return false;
    }

    return true;
  }

  // Addiction functions
  void add32(uint32_t n) {
    if (is64()) {
      add64(n);
      return;
    }

    uint32_t carryFlag = 0;
    addc32(reinterpret_cast<uint32_t*>(&Data_[0]), n, &carryFlag);
    for (size_t i = 1; i < LimbsNum; i++)
      addc32(reinterpret_cast<uint32_t*>(&Data_[i]), 0, &carryFlag);
  }

  void add64(uint64_t n) {
    if (is64()) {
      uint64_t carryFlag = 0;
      addc64(reinterpret_cast<uint64_t*>(&Data_[0]), n, &carryFlag);
      for (size_t i = 1; i < LimbsNum; i++)
        addc64(reinterpret_cast<uint64_t*>(&Data_[i]), 0, &carryFlag);
    } else {
      uint32_t carryFlag = 0;
      addc32(reinterpret_cast<uint32_t*>(&Data_[0]), n, &carryFlag);
      addc32(reinterpret_cast<uint32_t*>(&Data_[1]), n >> 32, &carryFlag);
      for (size_t i = 2; i < LimbsNum; i++)
        addc32(reinterpret_cast<uint32_t*>(&Data_[i]), 0, &carryFlag);
    }
  }

  template<unsigned OperandBits>
  void add(const UInt<OperandBits> &n) {
    LimbTy carry = 0;
    size_t minSize = std::min(LimbsNum, n.LimbsNum);
    for (size_t i = 0; i < minSize; i++)
      addc(&Data_[i], n.Data_[i], &carry);
    for (size_t i = minSize; i < LimbsNum; i++)
      addc(&Data_[i], 0, &carry);
  }

  // Substraction functions
  void sub32(uint32_t n) {
    if (is64()) {
      sub64(n);
      return;
    }

    uint32_t borrowFlag = 0;
    subb32(reinterpret_cast<uint32_t*>(&Data_[0]), n, &borrowFlag);
    for (size_t i = 1; i < LimbsNum; i++)
      subb32(reinterpret_cast<uint32_t*>(&Data_[i]), 0, &borrowFlag);
  }

  void sub64(uint64_t n) {
    if (is64()) {
      uint64_t borrowFlag = 0;
      subb64(reinterpret_cast<uint64_t*>(&Data_[0]), n, &borrowFlag);
      for (size_t i = 1; i < LimbsNum; i++)
        subb64(reinterpret_cast<uint64_t*>(&Data_[i]), 0, &borrowFlag);
    } else {
      uint32_t borrowFlag = 0;
      subb32(reinterpret_cast<uint32_t*>(&Data_[0]), n, &borrowFlag);
      subb32(reinterpret_cast<uint32_t*>(&Data_[1]), n >> 32, &borrowFlag);
      for (size_t i = 2; i < LimbsNum; i++)
        subb32(reinterpret_cast<uint32_t*>(&Data_[i]), 0, &borrowFlag);
    }
  }

  template<unsigned OperandBits>
  void sub(const UInt<OperandBits> &n) {
    LimbTy borrow = 0;
    size_t minSize = std::min(LimbsNum, n.LimbsNum);
    for (size_t i = 0; i < minSize; i++)
      subb(&Data_[i], n.Data_[i], &borrow);
    for (size_t i = minSize; i < LimbsNum; i++)
      subb(&Data_[i], 0, &borrow);
  }

  // Multiplication
  void mul32(uint32_t n) {
    if (is64()) {
      mul64(n);
      return;
    }

    uint32_t oldHi = 0;
    uint32_t carry = 0;
    for (size_t i = 0; i < LimbsNum; i++) {
      uint32_t hi;
      mulx32(Data_[i], n, reinterpret_cast<uint32_t*>(&Data_[i]), &hi);
      addc32(reinterpret_cast<uint32_t*>(&Data_[i]), oldHi, &carry);
      oldHi = hi;
    }
  }

  void mul64(uint64_t n) {
    if (is64()) {
      uint64_t oldHi = 0;
      uint64_t carry = 0;
      for (size_t i = 0; i < LimbsNum; i++) {
        uint64_t hi;
        mulx64(Data_[i], n, reinterpret_cast<uint64_t*>(&Data_[i]), &hi);
        addc64(reinterpret_cast<uint64_t*>(&Data_[i]), oldHi, &carry);
        oldHi = hi;
      }
    } else {
      assert("not implemented yet");
    }
  }

  template<unsigned OperandBits>
  void mul(const UInt<OperandBits> &n) {
    // Use product scan
    UInt<Bits> result;
    LimbTy accLow = 0;
    LimbTy accHi = 0;
    LimbTy carry = 0;
    for (size_t i = 0; i < LimbsNum; i++) {
      int off1 = std::max(static_cast<int>(i) - static_cast<int>(n.LimbsNum) + 1, 0);
      int off2 = i - off1;
      int count = std::min(static_cast<int>(LimbsNum)-off1, static_cast<int>(i)-off1+1);
      for (int j = 0; j < count; j++, off1++, off2--) {
        LimbTy lo;
        LimbTy hi;
        LimbTy lcarry = 0;
        mulx(Data_[off1], n.Data_[off2], &lo, &hi);
        addc(&accLow, lo, &lcarry);
        addc(&accHi, hi, &lcarry);
        addc(&carry, 0, &lcarry);
      }

      result.Data_[i] = accLow;
      accLow = accHi;
      accHi = carry;
      carry = 0;
    }

    memcpy(Data_, result.Data_, sizeof(LimbTy)*LimbsNum);
  }

  // Division
  void div32(uint32_t n) {
    if (is64()) {
      div64(n);
      return;
    }

    uint32_t mod = 0;
    for (size_t i = 0; i < LimbsNum; i++)
      divmod32(Data_[LimbsNum - i - 1], mod, n, reinterpret_cast<uint32_t*>(&Data_[LimbsNum - i - 1]), &mod);
  }

  void div64(uint64_t n) {
    if (is64()) {
      uint64_t mod = 0;
      for (size_t i = 0; i < LimbsNum; i++)
        divmod64(Data_[LimbsNum - i - 1], mod, n, reinterpret_cast<uint64_t*>(&Data_[LimbsNum - i - 1]), &mod);
    } else {
      assert("not implemented yet");
    }
  }

  // operator overload
  template<unsigned OperandBits> friend class UInt;

  // comparision
  bool operator==(uint32_t n) { return cmpeq32(n); }
  bool operator==(uint64_t n) { return cmpeq64(n); }
  template<unsigned OperandBits>
    bool operator==(const UInt<OperandBits> &n) { return cmpeq(n); }

  bool operator!=(uint32_t n) { return !cmpeq32(n); }
  bool operator!=(uint64_t n) { return !cmpeq64(n); }
  template<unsigned OperandBits>
    bool operator!=(const UInt<OperandBits> &n) { return !cmpeq(n); }

  // addition
  UInt<Bits> operator+=(uint32_t n) { add32(n); return *this; }
  UInt<Bits> operator+=(uint64_t n) { add64(n); return *this; }
  template<unsigned OperandBits>
    UInt<Bits> operator+=(const UInt<OperandBits> &n) { add(n); return *this; }
  UInt<Bits> friend operator+(const UInt<Bits> &a, uint32_t b) { return UInt<Bits>(a) += b; }
  UInt<Bits> friend operator+(const UInt<Bits> &a, uint64_t b) { return UInt<Bits>(a) += b; }
  template<unsigned OperandBits>
    friend UInt<Bits> operator+(const UInt<Bits> &a, const UInt<OperandBits> &b) { return UInt<Bits>(a) += b; }

  // substraction
  UInt<Bits> operator-=(uint32_t n) { sub32(n); return *this; }
  UInt<Bits> operator-=(uint64_t n) { sub64(n); return *this; }
  template<unsigned OperandBits>
    UInt<Bits> operator-=(const UInt<OperandBits> &n) { sub(n); return *this; }
  UInt<Bits> friend operator-(const UInt<Bits> &a, uint32_t b) { return UInt<Bits>(a) -= b; }
  UInt<Bits> friend operator-(const UInt<Bits> &a, uint64_t b) { return UInt<Bits>(a) -= b; }
  template<unsigned OperandBits>
    friend UInt<Bits> operator-(const UInt<Bits> &a, const UInt<OperandBits> &b) { return UInt<Bits>(a) -= b; }

  // multiplication
  UInt<Bits> operator*=(uint32_t n) { mul32(n); return *this; }
  UInt<Bits> operator*=(uint64_t n) { mul64(n); return *this; }
  template<unsigned OperandBits>
    UInt<Bits> operator*=(const UInt<OperandBits> &n) { mul(n); return *this; }
  UInt<Bits> friend operator*(const UInt<Bits> &a, uint32_t b) { return UInt<Bits>(a) *= b; }
  UInt<Bits> friend operator*(const UInt<Bits> &a, uint64_t b) { return UInt<Bits>(a) *= b; }
  template<unsigned OperandBits>
    friend UInt<Bits> operator*(const UInt<Bits> &a, const UInt<OperandBits> &b) { return UInt<Bits>(a) *= b; }

  // division
  UInt<Bits> operator/=(uint32_t n) { div32(n); return *this; }
  UInt<Bits> operator/=(uint64_t n) { div64(n); return *this; }
  UInt<Bits> friend operator/(const UInt<Bits> &a, uint32_t b) { return UInt<Bits>(a) /= b; }
  UInt<Bits> friend operator/(const UInt<Bits> &a, uint64_t b) { return UInt<Bits>(a) /= b; }

private:
  static constexpr size_t LimbSize = 8 * sizeof(LimbTy);
  static constexpr size_t LimbsNum = Bits / 8 / sizeof(LimbTy);
  static_assert(LimbsNum >= 2);

private:
  constexpr bool is64() const { return sizeof(LimbTy) == 8; }

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

  bool isSingleLimb() {
    for (size_t i = 1; i < LimbsNum; i++) {
      if (Data_[i] != 0)
        return false;
    }

    return true;
  }

  bool isDoubleLimb() {
    for (size_t i = 2; i < LimbsNum; i++) {
      if (Data_[i] != 0)
        return false;
    }

    return true;
  }

  void addc(LimbTy *a, LimbTy b, LimbTy *carry) {
    if (sizeof(LimbTy) == 8)
      addc64(reinterpret_cast<uint64_t*>(a), b, reinterpret_cast<uint64_t*>(carry));
    else if (sizeof(LimbTy) == 4)
      addc32(reinterpret_cast<uint32_t*>(a), b, reinterpret_cast<uint32_t*>(carry));
  }

  void subb(LimbTy *a, LimbTy b, LimbTy *borrow) {
    if (sizeof(LimbTy) == 8)
      subb64(reinterpret_cast<uint64_t*>(a), b, reinterpret_cast<uint64_t*>(borrow));
    else if (sizeof(LimbTy) == 4)
      subb32(reinterpret_cast<uint32_t*>(a), b, reinterpret_cast<uint32_t*>(borrow));
  }

  void mulx(LimbTy a, LimbTy b, LimbTy *lo, LimbTy *hi) {
    if (sizeof(LimbTy) == 8)
      mulx64(a, b, reinterpret_cast<uint64_t*>(lo), reinterpret_cast<uint64_t*>(hi));
    else if (sizeof(LimbTy) == 4)
      mulx32(a, b, reinterpret_cast<uint32_t*>(lo), reinterpret_cast<uint32_t*>(hi));
  }

private:
  LimbTy Data_[LimbsNum];
};
