// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "bigNum.h"

void uint256ToBN(mpz_ptr bignum, const uint256 &N)
{
  mpz_import(bignum, 32 / sizeof(unsigned long), -1, sizeof(unsigned long), -1, 0, N.begin());
}


void uint256ToBN(mpz_class &bigNum, const uint256 &N)
{
  uint256ToBN(bigNum.get_mpz_t(), N);
}

void uint256FromBN(uint256 &N, mpz_srcptr bigNum)
{
  N.SetNull();
  int limbsNum = std::min(static_cast<int>(256/8/sizeof(mp_limb_t)), bigNum->_mp_size);
  mp_limb_t *out = reinterpret_cast<mp_limb_t*>(N.begin());
  for (int i = 0; i < limbsNum; i++)
    out[i] = bigNum->_mp_d[i];
}
