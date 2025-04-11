// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "poolcommon/uint256.h"
#include "blockmaker/sha256.h"

static inline uint256 calculateMerkleRoot(uint256 *hashes, size_t size)
{
  if (size) {
    size_t txNum = size;
    CCtxSha256 sha256;
    while (txNum > 1) {
      size_t iterNum = (txNum / 2) + (txNum % 2);
      for (size_t i = 0; i < iterNum; i++) {
        sha256Init(&sha256);
        sha256Update(&sha256, hashes[i*2].begin(), sizeof(uint256));
        sha256Update(&sha256, hashes[i*2+1 < txNum ? i*2+1 : i*2].begin(), sizeof(uint256));
        sha256Final(&sha256, hashes[i].begin());

        sha256Init(&sha256);
        sha256Update(&sha256, hashes[i].begin(), sizeof(uint256));
        sha256Final(&sha256, hashes[i].begin());
      }

      txNum = iterNum;
    }

    return hashes[0];
  } else {
    uint256 zero;
    zero.SetNull();
    return zero;
  }
}


static inline uint256 calculateMerkleRoot(uint256 hash, const uint256 *begin, size_t size)
{
  CCtxSha256 sha256;
  uint256 result(hash);

  for (size_t i = 0; i != size; ++i) {
    sha256Init(&sha256);
    sha256Update(&sha256, result.begin(), result.size());
    sha256Update(&sha256, begin[i].begin(), begin[i].size());
    sha256Final(&sha256, result.begin());

    sha256Init(&sha256);
    sha256Update(&sha256, result.begin(), result.size());
    sha256Final(&sha256, result.begin());
  }

  return result;
}

static inline uint256 calculateMerkleRoot(const void *data, size_t size, const std::vector<uint256> &merklePath)
{
  uint256 result;
  CCtxSha256 sha256;
  sha256Init(&sha256);
  sha256Update(&sha256, data, size);
  sha256Final(&sha256, result.begin());
  sha256Init(&sha256);
  sha256Update(&sha256, result.begin(), result.size());
  sha256Final(&sha256, result.begin());
  return calculateMerkleRoot(result, &merklePath[0], merklePath.size());
}

static inline void dumpMerkleTree(std::vector<uint256> &hashes, std::vector<uint256> &out)
{
  out.clear();
  if (hashes.size() < 2)
    return;

  size_t txNum = hashes.size();

  CCtxSha256 sha256;
  while (txNum > 1) {
    size_t iterNum = (txNum / 2) + (txNum % 2);
    // dump second hash
    out.push_back(hashes[1]);
    for (size_t i = 0; i < iterNum; i++) {
      sha256Init(&sha256);
      sha256Update(&sha256, hashes[i*2].begin(), sizeof(uint256));
      sha256Update(&sha256, hashes[i*2+1 < txNum ? i*2+1 : i*2].begin(), sizeof(uint256));
      sha256Final(&sha256, hashes[i].begin());

      sha256Init(&sha256);
      sha256Update(&sha256, hashes[i].begin(), sizeof(uint256));
      sha256Final(&sha256, hashes[i].begin());
    }

    txNum = iterNum;
  }
}
