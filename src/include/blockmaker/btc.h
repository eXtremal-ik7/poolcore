#pragma once

#include <stdint.h>
#include <string.h>
#include <string>
#include "blockmaker/serialize.h"
#include "poolcommon/uint256.h"
#include <openssl/sha.h>
#include "rapidjson/document.h"
#include "poolinstances/stratumMsg.h"
#include "loguru.hpp"
#include "poolcommon/intrusive_ptr.h"
#include "poolcore/poolCore.h"

struct CCoinInfo;
struct PoolBackendConfig;
struct PoolBackend;

namespace BTC {
namespace Script {
  enum {
    OP_0 = 0,
    OP_RETURN = 0x6A,
    OP_DUP = 0x76,
    OP_EQUALVERIFY = 0x88,
    OP_HASH160 = 0xA9,
    OP_CHECKSIG = 0xAC
  };
}

class Proto {
public:
  static constexpr const char *TickerName = "BTC";

  using BlockHashTy = ::uint256;
  using TxHashTy = ::uint256;
  using AddressTy = ::uint160;

#pragma pack(push, 1)
  struct BlockHeader {
    uint32_t nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;

    BlockHashTy GetHash() const {
      uint256 result;
      SHA256_CTX sha256;
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, this, sizeof(*this));
      SHA256_Final(result.begin(), &sha256);

      SHA256_Init(&sha256);
      SHA256_Update(&sha256, result.begin(), sizeof(result));
      SHA256_Final(result.begin(), &sha256);
      return result;
    }
  };
#pragma pack(pop)

  struct TxIn {
    uint256 previousOutputHash;
    uint32_t previousOutputIndex;
    xvector<uint8_t> scriptSig;
    xvector<xvector<uint8_t>> witnessStack;
    uint32_t sequence;

    static bool unpackWitnessStack(xmstream &src, DynamicPtr<BTC::Proto::TxIn> dst) {
      BTC::unpack(src, DynamicPtr<decltype (dst->witnessStack)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxIn, witnessStack)));
      return !dst->witnessStack.empty();
    }

    size_t scriptSigOffset();
  };

  struct TxOut {
    int64_t value;
    xvector<uint8_t> pkScript;
  };

  struct Transaction {
    int32_t version;
    xvector<TxIn> txIn;
    xvector<TxOut> txOut;
    uint32_t lockTime;

    // Memory only
    uint32_t SerializedDataOffset = 0;
    uint32_t SerializedDataSize = 0;
    BTC::Proto::TxHashTy Hash;

    bool hasWitness() const {
      for (size_t i = 0; i < txIn.size(); i++) {
        if (!txIn[i].witnessStack.empty())
          return true;
      }

      return false;
    }

    BlockHashTy GetHash() {
      if (!Hash.IsNull())
         return Hash;

      uint256 result;
      uint8_t buffer[4096];
      xmstream stream(buffer, sizeof(buffer));
      stream.reset();
      BTC::serialize(stream, *this);

      SHA256_CTX sha256;
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, stream.data(), stream.sizeOf());
      SHA256_Final(result.begin(), &sha256);
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, result.begin(), sizeof(result));
      SHA256_Final(result.begin(), &sha256);
      Hash = result;
      return result;
    }

    // Suitable for mining
    size_t getFirstScriptSigOffset(bool serializeWitness);
  };

  struct TxWitness {
    std::vector<uint8_t> data;
  };

  template<typename T>
  struct BlockTy {
    typename T::BlockHeader header;
    xvector<typename T::Transaction> vtx;
    size_t getTransactionsNum() { return vtx.size(); }
    BlockHashTy getHash() { return header.GetHash(); }
  };

  using Block = BlockTy<BTC::Proto>;
  using MessageBlock = Block;

  // Consensus (PoW)
  struct CheckConsensusCtx {};
  struct ChainParams {
    uint256 powLimit;
  };

  static void checkConsensusInitialize(CheckConsensusCtx&) {}
  static bool checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx&, ChainParams&, double *shareDiff);
  static bool checkConsensus(const Proto::Block &block, CheckConsensusCtx &ctx, ChainParams &params, double *shareDiff) { return checkConsensus(block.header, ctx, params, shareDiff); }
};
}

namespace BTC {

// Header
template<> struct Io<Proto::BlockHeader> {
  static void serialize(xmstream &dst, const BTC::Proto::BlockHeader &data);
  static void unserialize(xmstream &src, BTC::Proto::BlockHeader &data);
  static void unpack(xmstream &src, DynamicPtr<BTC::Proto::BlockHeader> dst) { unserialize(src, *dst.ptr()); }
  static void unpackFinalize(DynamicPtr<BTC::Proto::BlockHeader>) {}
};

// TxIn
template<> struct Io<Proto::TxIn> {
  static void serialize(xmstream &dst, const BTC::Proto::TxIn &data);
  static void unserialize(xmstream &src, BTC::Proto::TxIn &data);
  static void unpack(xmstream &src, DynamicPtr<BTC::Proto::TxIn> dst);
  static void unpackFinalize(DynamicPtr<BTC::Proto::TxIn> dst);
};

// TxOut
template<> struct Io<Proto::TxOut> {
  static void serialize(xmstream &dst, const BTC::Proto::TxOut &data);
  static void unserialize(xmstream &src, BTC::Proto::TxOut &data);
  static void unpack(xmstream &src, DynamicPtr<BTC::Proto::TxOut> dst);
  static void unpackFinalize(DynamicPtr<BTC::Proto::TxOut> dst);
};

// Transaction
template<> struct Io<Proto::Transaction> {
  static void serialize(xmstream &dst, const BTC::Proto::Transaction &data, bool serializeWitness=true);
  static void unserialize(xmstream &src, BTC::Proto::Transaction &data);
  static void unpack(xmstream &src, DynamicPtr<BTC::Proto::Transaction> dst);
  static void unpackFinalize(DynamicPtr<BTC::Proto::Transaction> dst);
};

// Block
template<typename T> struct Io<Proto::BlockTy<T>> {
  static inline void serialize(xmstream &dst, const BTC::Proto::BlockTy<T> &data) {
    BTC::serialize(dst, data.header);
    BTC::serialize(dst, data.vtx);
  }

  static inline void unserialize(xmstream &src, BTC::Proto::BlockTy<T> &data) {
    size_t blockDataOffset = src.offsetOf();

    BTC::unserialize(src, data.header);

    uint64_t txNum = 0;
    unserializeVarSize(src, txNum);
    if (txNum > src.remaining()) {
      src.seekEnd(0, true);
      return;
    }

    data.vtx.resize(txNum);
    for (uint64_t i = 0; i < txNum; i++) {
      data.vtx[i].SerializedDataOffset = static_cast<uint32_t>(src.offsetOf() - blockDataOffset);
      BTC::unserialize(src, data.vtx[i]);
      data.vtx[i].SerializedDataSize = static_cast<uint32_t>(src.offsetOf() - data.vtx[i].SerializedDataOffset);
    }
  }

  static inline void unpack(xmstream &src, DynamicPtr<BTC::Proto::BlockTy<T>> dst) {
    size_t blockDataOffset = src.offsetOf();
    BTC::unpack(src, DynamicPtr<decltype (dst->header)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, header)));

    {
      auto vtxDst = DynamicPtr<decltype (dst->vtx)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, vtx));
      uint64_t txNum;
      unserializeVarSize(src, txNum);
      size_t dataOffset = vtxDst.stream().offsetOf();
      vtxDst.stream().reserve(txNum*sizeof(typename T::Transaction));

      new (vtxDst.ptr()) xvector<typename T::Transaction>(reinterpret_cast<typename T::Transaction*>(dataOffset), txNum, false);
      for (uint64_t i = 0; i < txNum; i++) {
        auto txPtr = DynamicPtr<typename T::Transaction>(vtxDst.stream(), dataOffset + sizeof(typename T::Transaction)*i);
        txPtr->SerializedDataOffset = static_cast<uint32_t>(src.offsetOf() - blockDataOffset);
        BTC::unpack(src, txPtr);
        txPtr->SerializedDataSize = static_cast<uint32_t>(src.offsetOf() - txPtr->SerializedDataOffset);
      }
    }
  }

  static inline void unpackFinalize(DynamicPtr<BTC::Proto::BlockTy<T>> dst) {
    BTC::unpackFinalize(DynamicPtr<decltype (dst->header)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, header)));
    BTC::unpackFinalize(DynamicPtr<decltype (dst->vtx)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, vtx)));
  }
};

class Stratum {
public:
  static constexpr double DifficultyFactor = 1.0;

  struct MiningConfig {
    unsigned FixedExtraNonceSize = 4;
    unsigned MutableExtraNonceSize = 4;
    void initialize(rapidjson::Value &instanceCfg);
  };

  struct ThreadConfig {
    uint64_t ExtraNonceCurrent;
    unsigned ThreadsNum;
    void initialize(unsigned instanceId, unsigned instancesNum);
  };

  struct WorkerConfig {
    std::string SetDifficultySession;
    std::string NotifySession;
    uint64_t ExtraNonceFixed;
    bool AsicBoostEnabled = false;
    uint32_t VersionMask = 0;
    void initialize(ThreadConfig &threadCfg);
    void setupVersionRolling(uint32_t versionMask);
    void onSubscribe(MiningConfig &miningCfg, StratumMessage &msg, xmstream &out);
  };

  class Work {
  public:
    uint64_t UniqueWorkId;
    uint64_t Height = 0;
    PoolBackend *Backend = nullptr;

    struct {
      xmstream Data;
      unsigned ExtraDataOffset;
      unsigned ExtraNonceOffset;
    } CBTxLegacy;

    struct {
      xmstream Data;
      unsigned ExtraDataOffset;
      unsigned ExtraNonceOffset;
    } CBTxWitness;

    unsigned BlockHexCoinbaseTxOffset;

    Proto::BlockHeader Header;
    std::vector<uint256> MerklePath;
    uint32_t JobVersion;
    size_t TxNum;
    int64_t BlockReward;
    xmstream BlockHexData;
    xmstream NotifyMessage;

  public:
    size_t backendsNum() { return 1; }
    uint256 hash() { return Header.GetHash(); }
    PoolBackend *backend(size_t) { return Backend; }
    uint64_t height(size_t) { return Height; }
    size_t txNum(size_t) { return TxNum; }
    int64_t blockReward(size_t) { return BlockReward; }
    const xmstream &notifyMessage() { return NotifyMessage; }
    const xmstream &blockHexData(size_t) { return BlockHexData; }
    bool initialized() { return Backend != nullptr; }

    bool isNewBlock(const PoolBackend *backend, const std::string&, uint64_t workId) {
      // Profit switcher: don't reset previous work
      if (backend != Backend)
        return false;
      return UniqueWorkId != workId;
    }

    bool checkConsensus(size_t, double *shareDiff) {
      Proto::CheckConsensusCtx ctx;
      Proto::ChainParams params;
      Proto::checkConsensusInitialize(ctx);
      return Proto::checkConsensus(Header, ctx, params, shareDiff);
    }

    void buildNotifyMessage(MiningConfig &cfg, uint64_t majorJobId, unsigned minorJobId, bool resetPreviousWork);

    bool loadFromTemplate(rapidjson::Value &document,
                          uint64_t uniqueWorkId,
                          const MiningConfig &cfg,
                          PoolBackend *backend,
                          const std::string &ticker,
                          Proto::AddressTy &miningAddress,
                          const void *coinBaseExtraData,
                          size_t coinbaseExtraSize,
                          std::string &error,
                          size_t txNumLimit=0);

    bool prepareForSubmit(const WorkerConfig &workerCfg, const MiningConfig &miningCfg, const StratumMessage &msg);
  };

  static bool suitableForProfitSwitcher(const std::string&) { return true; }
  static double getIncomingProfitValue(rapidjson::Value &document, double price, double coeff);
};

struct X {
  using Proto = BTC::Proto;
  using Stratum = BTC::Stratum;

  template<typename T> static inline void serialize(xmstream &src, const T &data) { Io<T>::serialize(src, data); }
  template<typename T> static inline void unserialize(xmstream &dst, T &data) { Io<T>::unserialize(dst, data); }
};
}

void serializeJsonInside(xmstream &stream, const BTC::Proto::BlockHeader &header);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::TxIn &txin);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::TxOut &txout);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::Transaction &data);
bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, BTC::Proto::AddressTy &address);
