// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "blockmaker/btc.h"
#include "poolcommon/bigNum.h"
#include "poolinstances/protocol.pb.h"

namespace XPM {
class Proto {
public:
  static constexpr const char *TickerName = "XPM";

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
  using Transaction = BTC::Proto::Transaction;
  using Block = BTC::Proto::BlockTy<XPM::Proto>;

  static bool loadHeaderFromTemplate(Proto::BlockHeader &header, rapidjson::Value &blockTemplate);

  // Consensus (PoW)
  struct CheckConsensusCtx {
    mpz_t bnPrimeChainOrigin;
    mpz_t bn;
    mpz_t exp;
    mpz_t EulerResult;
    mpz_t FermatResult;
    mpz_t two;
  };

  struct ChainParams {
    // XPM specific
    uint32_t minimalChainLength;
  };


  static void checkConsensusInitialize(CheckConsensusCtx &ctx);
  static bool checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams, uint64_t *shareSize);
};

struct Zmq {
public:
  class Work {
  public:
    uint64_t Height = 0;
    PoolBackend *Backend = nullptr;
    unsigned ScriptSigExtraNonceOffset;
    unsigned TxExtraNonceOffset;
    unsigned BlockHexExtraNonceOffset;
    size_t TransactionsNum;
    int64_t BlockReward;

    Proto::BlockHeader Header;
    std::vector<uint256> MerklePath;
    xmstream FirstTxData;
    xmstream BlockHexData;

  public:
    PoolBackend *backend() { return Backend; }
    uint64_t height() { return Height; }
    uint256 hash() { return Header.GetHash(); }
    size_t txNum() { return TransactionsNum; }
    int64_t blockReward() { return BlockReward; }
    const xmstream &blockHexData() { return BlockHexData; }

    bool checkConsensus(Proto::CheckConsensusCtx &ctx, uint64_t *shareDiff) {
      Proto::ChainParams params;
      params.minimalChainLength = 2;
      return XPM::Proto::checkConsensus(Header, ctx, params, shareDiff);
    }
  };

  struct MiningConfig {
    unsigned FixedExtraNonceSize = 8;
    unsigned MinShareLength = 7;
  };

  struct ThreadConfig {
    std::unordered_map<uint256, uint64_t> ExtraNonceMap;
    uint64_t ExtraNonceCurrent;
    unsigned ThreadsNum;
  };

  struct WorkerConfig {
    uint64_t ExtraNonceFixed;
  };

public:
  static void initializeMiningConfig(MiningConfig &cfg, rapidjson::Value &instanceCfg);
  static void initializeThreadConfig(ThreadConfig &cfg, unsigned threadId, unsigned threadsNum);
  static void resetThreadConfig(ThreadConfig &cfg);

  static void buildBlockProto(Work &work, MiningConfig &miningCfg, pool::proto::Block &proto);
  static void buildWorkProto(Work &work, pool::proto::Work &proto);

  static bool loadFromTemplate(Work &work, rapidjson::Value &blockTemplate, const MiningConfig &cfg, PoolBackend *backend, const std::string&, Proto::AddressTy &miningAddress, const std::string &coinbaseMsg, std::string &error);
  static void generateNewWork(Work &work, WorkerConfig &workerCfg, ThreadConfig &threadCfg, MiningConfig &miningCfg);
  static bool prepareToSubmit(Work &work, ThreadConfig &threadCfg, MiningConfig &miningCfg, pool::proto::Request &req, pool::proto::Reply &rep);
};

struct X {
  using Proto = XPM::Proto;
  using Zmq = XPM::Zmq;
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

void serializeJsonInside(xmstream &stream, const XPM::Proto::BlockHeader &header);
