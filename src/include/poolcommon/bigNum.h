// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once
#include "uint256.h"
#include <mpirxx.h>

void uint256ToBN(mpz_ptr bignum, const uint256 &N);
void uint256ToBN(mpz_class &bigNum, const uint256 &N);
void uint256FromBN(uint256 &N, mpz_srcptr bigNum);
