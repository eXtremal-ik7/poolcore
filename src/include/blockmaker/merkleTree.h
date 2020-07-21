// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "blockmaker/xvector.h"
#include "poolcommon/uint256.h"
#include "openssl/sha.h"
#include <memory>

template<typename Proto>
typename Proto::BlockHashTy calculateMerkleRoot(const xvector<typename Proto::Transaction> &vtx)
{
  size_t txNum = vtx.size();
  std::unique_ptr<typename Proto::BlockHashTy[]> hashes(new typename Proto::BlockHashTy[txNum]);

  // Get hashes for all transactions
  for (size_t i = 0; i < txNum; i++)
    hashes[i] = vtx[i].GetHash();

  SHA256_CTX sha256;
  while (txNum > 1) {
    size_t iterNum = (txNum / 2) + (txNum % 2);
    for (size_t i = 0; i < iterNum; i++) {
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, hashes[i*2].begin(), sizeof(uint256));
      SHA256_Update(&sha256, hashes[i*2+1 < txNum ? i*2+1 : i*2].begin(), sizeof(uint256));
      SHA256_Final(hashes[i].begin(), &sha256);

      SHA256_Init(&sha256);
      SHA256_Update(&sha256, hashes[i].begin(), sizeof(uint256));
      SHA256_Final(hashes[i].begin(), &sha256);
    }

    txNum = iterNum;
  }

  return hashes[0];
}
