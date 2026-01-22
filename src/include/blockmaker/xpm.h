// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "blockmaker/btc.h"
#include "blockmaker/divisionChecker.h"
#include "poolcommon/bigNum.h"
#include "poolinstances/protocol.pb.h"
#include "blockmaker/sha256.h"
#include <deque>

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
    BaseBlob<256> hashPrevBlock;
    BaseBlob<256> hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;
    mpz_class bnPrimeChainMultiplier;

    BlockHashTy GetHash() const {
      uint8_t buffer[256];
      BaseBlob<256> result;
      xmstream localStream(buffer, sizeof(buffer));
      localStream.reset();
      BTC::serialize(localStream, bnPrimeChainMultiplier);

      CCtxSha256 sha256;
      sha256Init(&sha256);
      sha256Update(&sha256, this, 4+32+32+4+4+4);
      sha256Update(&sha256, localStream.data(), localStream.sizeOf());
      sha256Final(&sha256, result.begin());

      sha256Init(&sha256);
      sha256Update(&sha256, result.begin(), sizeof(result));
      sha256Final(&sha256, result.begin());
      return result;
    }

    UInt<256> GetOriginalHeaderHash() const {
      UInt<256>  result;
      CCtxSha256 sha256;
      sha256Init(&sha256);
      sha256Update(&sha256, this, 4+32+32+4+4+4);
      sha256Final(&sha256, reinterpret_cast<uint8_t*>(result.data()));

      sha256Init(&sha256);
      sha256Update(&sha256, result.data(), sizeof(result));
      sha256Final(&sha256, reinterpret_cast<uint8_t*>(result.data()));
      for (unsigned i = 0; i < 4; i++)
        result.data()[i] = readle(result.data()[i]);
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

  enum EChainType {
    ECunninghamChain1 = 1,
    ECunninghamChain2 = 2,
    EBiTwin = 3
  };

  static void checkConsensusInitialize(CheckConsensusCtx &ctx);

  static bool checkConsensus(const Proto::BlockHeader &header,
                             CheckConsensusCtx &ctx,
                             ChainParams &chainParams,
                             double *shareSize,
                             EChainType *chainType);

  static bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, AddressTy &address) { return BTC::Proto::decodeHumanReadableAddress(hrAddress, pubkeyAddressPrefix, address); }
};

struct Zmq {
  struct WorkerContext;

public:
  struct MiningConfig {
    unsigned FixedExtraNonceSize = 8;
    unsigned MinShareLength = 7;
  };

  struct ThreadConfig {
    std::unordered_map<BaseBlob<256>, uint64_t> ExtraNonceMap;
    uint64_t ExtraNonceCurrent;
    unsigned ThreadsNum;
  };

  struct WorkerConfig {
    uint64_t ExtraNonceFixed;
    unsigned WeaveDepth = 0;
    std::deque<uint32_t> SharesBitSize;

    void initialize(ThreadConfig &threadCfg) {
      // Set fixed part of extra nonce
      ExtraNonceFixed = threadCfg.ExtraNonceCurrent;
      // Update thread config
      threadCfg.ExtraNonceCurrent += threadCfg.ThreadsNum;
    }

    bool setConfig(unsigned weaveDepth) {
      if (weaveDepth == 0 || weaveDepth > 131072)
        return false;

      WeaveDepth = weaveDepth;
      return true;
    }
    bool hasConfig() { return WeaveDepth != 0; }
  };

  class Work {
  public:
    struct CExtraInfo {
      Proto::EChainType ChainType;
    };

  public:
    uint64_t Height = 0;
    PoolBackend *Backend = nullptr;
    unsigned ScriptSigExtraNonceOffset;
    unsigned TxExtraNonceOffset;
    unsigned BlockHexExtraNonceOffset;
    size_t TransactionsNum;
    int64_t BlockReward;

    Proto::BlockHeader Header;
    std::vector<BaseBlob<256>> MerklePath;
    xmstream CoinbaseTx;
    xmstream TxHexData;
    xmstream BlockHexData;

  public:
    PoolBackend *backend() { return Backend; }
    uint64_t height() { return Height; }
    BaseBlob<256> hash() { return Header.GetHash(); }
    size_t txNum() { return TransactionsNum; }
    int64_t blockReward() { return BlockReward; }
    const xmstream &blockHexData() { return BlockHexData; }
    double expectedWork();

    bool checkConsensus(Proto::CheckConsensusCtx &ctx, double *shareDiff, CExtraInfo *info) {
      Proto::ChainParams params;
      params.minimalChainLength = 2;
      return XPM::Proto::checkConsensus(Header, ctx, params, shareDiff, &info->ChainType);
    }

    double shareWork(Proto::CheckConsensusCtx &ctx, double shareDiff, double shareTarget, const CExtraInfo &info, WorkerConfig &workerConfig);
    uint32_t primePOWTarget();
  };

public:
  static void initialize() { DivisionChecker_.init(131072 + 1024, 2048/32); }
  static void initializeMiningConfig(MiningConfig &cfg, rapidjson::Value &instanceCfg);
  static void initializeThreadConfig(ThreadConfig &cfg, unsigned threadId, unsigned threadsNum);
  static void resetThreadConfig(ThreadConfig &cfg);

  static void buildBlockProto(Work &work, MiningConfig &miningCfg, pool::proto::Block &proto);
  static void buildWorkProto(Work &work, pool::proto::Work &proto);

  static bool loadFromTemplate(Work &work, rapidjson::Value &blockTemplate, const MiningConfig &cfg, PoolBackend *backend, const std::string&, Proto::AddressTy &miningAddress, const std::string &coinbaseMsg, std::string &error);
  static void generateNewWork(Work &work, WorkerConfig &workerCfg, ThreadConfig &threadCfg, MiningConfig &miningCfg);
  static bool prepareToSubmit(Work &work, ThreadConfig &threadCfg, MiningConfig &miningCfg, pool::proto::Request &req, pool::proto::Reply &rep);

public:
  static CDivisionChecker DivisionChecker_;
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
