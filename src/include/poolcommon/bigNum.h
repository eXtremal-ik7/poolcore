// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once
#include "endiantools.h"
#include "uint.h"
#include <gmpxx.h>

// UInt -> mpz_class
template<unsigned Bits>
void uintToMpz(mpz_ptr mpz, const UInt<Bits>& value)
{
  mpz_import(mpz, Bits / 64, -1, sizeof(uint64_t), 0, 0, value.data());
}

template<unsigned Bits>
mpz_class uintToMpz(const UInt<Bits>& value)
{
  mpz_class result;
  mpz_import(result.get_mpz_t(), Bits / 64, -1, sizeof(uint64_t), 0, 0, value.data());
  return result;
}

template<unsigned Bits>
UInt<Bits> mpzToUint(mpz_srcptr value) {
  UInt<Bits> result = UInt<Bits>::zero();

  size_t gmpLimbs = mpz_size(value);
  if (gmpLimbs == 0)
    return result;

  const mp_limb_t* src = mpz_limbs_read(value);
  constexpr size_t uintLimbs = Bits / 64;

  if constexpr (sizeof(mp_limb_t) == sizeof(uint64_t)) {
    size_t copyLimbs = std::min(gmpLimbs, uintLimbs);
    memcpy(result.data(), src, copyLimbs * sizeof(uint64_t));
  } else if constexpr (sizeof(mp_limb_t) == sizeof(uint32_t)) {
    if (isLittleEndian()) {
      size_t copyBytes = std::min(gmpLimbs * 4, uintLimbs * 8);
      memcpy(result.data(), src, copyBytes);
    } else {
      for (size_t i = 0; i < uintLimbs; ++i) {
        size_t srcIdx = i * 2;
        uint32_t lo = (srcIdx < gmpLimbs) ? src[srcIdx] : 0;
        uint32_t hi = (srcIdx + 1 < gmpLimbs) ? src[srcIdx + 1] : 0;
        result.data()[i] = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
      }
    }
  } else {
    static_assert(sizeof(mp_limb_t) == 4 || sizeof(mp_limb_t) == 8, "Unsupported GMP limb size");
  }

  return result;
}

// mpz_class -> UInt
template<unsigned Bits>
UInt<Bits> mpzToUint(const mpz_class& value)
{
  return mpzToUint<Bits>(value.get_mpz_t());
}
