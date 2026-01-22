// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "poolcommon/baseBlob.h"
#include "blockmaker/sha256.h"
#include <memory>
#include <vector>

static inline BaseBlob<256> calculateMerkleRoot(const BaseBlob<256> *hashes, size_t size)
{
  if (size == 0) {
    BaseBlob<256> zero;
    zero.setNull();
    return zero;
  } else if (size == 1) {
    return hashes[0];
  }

  CCtxSha256 sha256;
  size_t allocationSize = size / 2 + (size % 2);
  std::unique_ptr<BaseBlob<256>[]> localHashes(new BaseBlob<256>[allocationSize]);
  for (size_t i = 0; i < allocationSize; i++) {
    sha256Init(&sha256);
    sha256Update(&sha256, hashes[i*2].begin(), sizeof(BaseBlob<256>));
    sha256Update(&sha256, hashes[i*2+1 < size ? i*2+1 : i*2].begin(), sizeof(BaseBlob<256>));
    sha256Final(&sha256, localHashes[i].begin());

    sha256Init(&sha256);
    sha256Update(&sha256, localHashes[i].begin(), sizeof(BaseBlob<256>));
    sha256Final(&sha256, localHashes[i].begin());
  }

  size_t txNum = allocationSize;
  while (txNum > 1) {
    size_t iterNum = (txNum / 2) + (txNum % 2);
    for (size_t i = 0; i < iterNum; i++) {
      sha256Init(&sha256);
      sha256Update(&sha256, localHashes[i*2].begin(), sizeof(BaseBlob<256>));
      sha256Update(&sha256, localHashes[i*2+1 < txNum ? i*2+1 : i*2].begin(), sizeof(BaseBlob<256>));
      sha256Final(&sha256, localHashes[i].begin());

      sha256Init(&sha256);
      sha256Update(&sha256, localHashes[i].begin(), sizeof(BaseBlob<256>));
      sha256Final(&sha256, localHashes[i].begin());
    }

    txNum = iterNum;
  }

  return localHashes[0];
}

static inline BaseBlob<256> calculateMerkleRootWithPath(BaseBlob<256> hash, const BaseBlob<256> *tree, size_t treeSize, size_t index)
{
  BaseBlob<256> result = hash;
  if (!treeSize)
    return result;

  CCtxSha256 sha256;
  for (size_t i = 0; i < treeSize; i++) {
    if (index & 1) {
      sha256Init(&sha256);
      sha256Update(&sha256, tree[i].begin(), sizeof(BaseBlob<256>));
      sha256Update(&sha256, result.begin(), sizeof(BaseBlob<256>));
      sha256Final(&sha256, result.begin());

      sha256Init(&sha256);
      sha256Update(&sha256, result.begin(), sizeof(BaseBlob<256>));
      sha256Final(&sha256, result.begin());
    } else {
      sha256Init(&sha256);
      sha256Update(&sha256, result.begin(), sizeof(BaseBlob<256>));
      sha256Update(&sha256, tree[i].begin(), sizeof(BaseBlob<256>));
      sha256Final(&sha256, result.begin());

      sha256Init(&sha256);
      sha256Update(&sha256, result.begin(), sizeof(BaseBlob<256>));
      sha256Final(&sha256, result.begin());
    }

    index >>= 1;
  }

  return result;
}

static inline void buildMerklePath(const std::vector<BaseBlob<256>> &hashes, size_t index, std::vector<BaseBlob<256>> &path)
{
  path.clear();
  if (hashes.size() < 2)
    return;

  CCtxSha256 sha256;
  size_t currentIndex = index;
  size_t allocationSize = hashes.size() / 2 + (hashes.size() % 2);
  std::unique_ptr<BaseBlob<256>[]> localHashes(new BaseBlob<256>[allocationSize]);
  path.push_back(hashes[std::min(currentIndex ^ 1, hashes.size() - 1)]);

  for (size_t i = 0; i < allocationSize; i++) {
    sha256Init(&sha256);
    sha256Update(&sha256, hashes[i*2].begin(), sizeof(BaseBlob<256>));
    sha256Update(&sha256, hashes[i*2+1 < hashes.size() ? i*2+1 : i*2].begin(), sizeof(BaseBlob<256>));
    sha256Final(&sha256, localHashes[i].begin());

    sha256Init(&sha256);
    sha256Update(&sha256, localHashes[i].begin(), sizeof(BaseBlob<256>));
    sha256Final(&sha256, localHashes[i].begin());
  }

  currentIndex >>= 1;

  size_t txNum = allocationSize;
  while (txNum > 1) {
    path.push_back(localHashes[std::min(currentIndex ^ 1, txNum - 1)]);

    size_t iterNum = (txNum / 2) + (txNum % 2);
    for (size_t i = 0; i < iterNum; i++) {
      sha256Init(&sha256);
      sha256Update(&sha256, localHashes[i*2].begin(), sizeof(BaseBlob<256>));
      sha256Update(&sha256, localHashes[i*2+1 < txNum ? i*2+1 : i*2].begin(), sizeof(BaseBlob<256>));
      sha256Final(&sha256, localHashes[i].begin());

      sha256Init(&sha256);
      sha256Update(&sha256, localHashes[i].begin(), sizeof(BaseBlob<256>));
      sha256Final(&sha256, localHashes[i].begin());
    }

    currentIndex >>= 1;
    txNum = iterNum;
  }
}
