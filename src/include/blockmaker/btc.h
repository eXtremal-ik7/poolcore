#pragma once

#include "btcLike.h"

struct CCoinInfo;
struct PoolBackendConfig;
class PoolBackend;

namespace BTC {
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
};

class Stratum {
public:
  static constexpr double DifficultyFactor = 1.0;

  // TODO: Use this for headers non-compatible with BTC
  struct TemplateLoader {};

  struct Notify {
    static void build(StratumWork *source, typename Proto::BlockHeader &header, uint32_t asicBoostData, CoinbaseTx &legacy, const std::vector<uint256> &merklePath, const MiningConfig &cfg, bool resetPreviousWork, xmstream &notifyMessage);
  };

  struct Prepare {
    static bool prepare(typename Proto::BlockHeader &header, uint32_t asicBoostData, CoinbaseTx &legacy, CoinbaseTx &witness, const std::vector<uint256> &merklePath, const CWorkerConfig &workerCfg, const MiningConfig &miningCfg, const StratumMessage &msg);
  };

  using Work = BTC::WorkTy<BTC::Proto, BTC::Stratum::TemplateLoader, BTC::Stratum::Notify, BTC::Stratum::Prepare>;
  using SecondWork = StratumSingleWorkEmpty;
  using MergedWork = StratumMergedWorkEmpty;

  static constexpr bool MergedMiningSupport = false;
  static bool isMainBackend(const std::string&) { return true; }
  static bool keepOldWorkForBackend(const std::string&) { return false; }
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
