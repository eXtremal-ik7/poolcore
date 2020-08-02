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
    Proto::Block Block;
    uint64_t Height;
    unsigned ScriptSigExtraNonceOffset;

  public:
    mutable std::atomic<uintptr_t> Refs_ = 0;
    uintptr_t ref_fetch_add(uintptr_t count) const { return Refs_.fetch_add(count); }
    uintptr_t ref_fetch_sub(uintptr_t count) const { return Refs_.fetch_sub(count); }
    Work() {}
    Work(const Work &work) : Block(work.Block), Height(work.Height), ScriptSigExtraNonceOffset(work.ScriptSigExtraNonceOffset), Refs_(0) {}
    Work &operator=(const Work &work) {
      Block = work.Block;
      Height = work.Height;
      ScriptSigExtraNonceOffset = work.ScriptSigExtraNonceOffset;
      Refs_ = 0;
      return *this;
    }
  };

  struct MiningConfig {
    unsigned FixedExtraNonceSize = 8;
    unsigned MinShareLength = 7;
    std::string CoinbaseMessage;
    BTC::Proto::AddressTy Address;
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
  static bool buildFullMiningConfig(MiningConfig &cfg, const PoolBackendConfig &backendCfg, const CCoinInfo &coinInfo);

  static void buildBlockProto(Work &work, WorkerConfig &workerCfg, ThreadConfig &threadCfg, MiningConfig &miningCfg, pool::proto::Block &proto);
  static void buildWorkProto(Work &work, WorkerConfig &workerCfg, ThreadConfig &threadCfg, MiningConfig &miningCfg, pool::proto::Work &proto);

  static intrusive_ptr<Work> loadFromTemplate(rapidjson::Value &blockTemplate, const MiningConfig &cfg, std::string &error);
  static void generateNewWork(Work &work, WorkerConfig &workerCfg, ThreadConfig &threadCfg, MiningConfig &miningCfg);
  static bool prepareToSubmit(Work &work, ThreadConfig &threadCfg, MiningConfig &miningCfg, pool::proto::Request &req, pool::proto::Reply &rep, int64_t *blockReward);
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
