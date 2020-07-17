// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "blockmaker/btc.h"
#include "poolcommon/bigNum.h"

namespace XPM {
class Proto {
public:
  using BlockHashTy = BTC::Proto::BlockHashTy;
  using TxHashTy = BTC::Proto::TxHashTy;
  using AddressTy = BTC::Proto::AddressTy;

  // Data structures
#pragma pack(push, 1)
  struct BlockHeader {
    int32_t nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;
    mpz_class bnPrimeChainMultiplier;

    BlockHashTy GetHash() const {
      uint8_t buffer[256];
      uint256 result;
      xmstream localStream(buffer, sizeof(buffer));
      localStream.reset();
      BTC::serialize(localStream, bnPrimeChainMultiplier);

      SHA256_CTX sha256;
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, this, 4+32+32+4+4+4);
      SHA256_Update(&sha256, localStream.data(), localStream.sizeOf());
      SHA256_Final(result.begin(), &sha256);

      SHA256_Init(&sha256);
      SHA256_Update(&sha256, result.begin(), sizeof(result));
      SHA256_Final(result.begin(), &sha256);
      return result;
    }

    BlockHashTy GetOriginalHeaderHash() const {
        uint256 result;
        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, this, 4+32+32+4+4+4);
        SHA256_Final(result.begin(), &sha256);

        SHA256_Init(&sha256);
        SHA256_Update(&sha256, result.begin(), sizeof(result));
        SHA256_Final(result.begin(), &sha256);
        return result;
    }
  };
#pragma pack(pop)

  using TxIn = BTC::Proto::TxIn;
  using TxOut = BTC::Proto::TxOut;
  using Transaction = BTC::Proto::TransactionTy<XPM::Proto>;
  using Block = BTC::Proto::BlockTy<XPM::Proto>;
};
}

// Serialize
namespace BTC {
template<> struct Io<mpz_class> {
  static void serialize(xmstream &dst, const mpz_class &data);
  static void unserialize(xmstream &src, mpz_class &data);
  static void unpack(xmstream &src, DynamicPtr<mpz_class> dst);
  static void unpackFinalize(DynamicPtr<mpz_class> dst);
};

template<> struct Io<XPM::Proto::BlockHeader> {
  static void serialize(xmstream &dst, const XPM::Proto::BlockHeader &data);
  static void unserialize(xmstream &src, XPM::Proto::BlockHeader &data);
  static void unpack(xmstream &src, DynamicPtr<XPM::Proto::BlockHeader> dst);
  static void unpackFinalize(DynamicPtr<XPM::Proto::BlockHeader> dst);
};

}
