#pragma once

#include "btcLike.h"
#include "poolcommon/uint.h"
#include "sha256.h"
#include "poolcommon/baseBlob.h"

struct CCoinInfo;
struct PoolBackendConfig;
class PoolBackend;

namespace BTC {
static inline double difficultyFromBits(uint32_t bits, unsigned shiftCount) {
    unsigned nShift = (bits >> 24) & 0xff;
    double dDiff =
        (double)0x0000ffff / (double)(bits & 0x00ffffff);

    while (nShift < shiftCount)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > shiftCount)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

class Proto {
public:
  static constexpr const char *TickerName = "BTC";

  using BlockHashTy = ::BaseBlob<256>;
  using TxHashTy = ::BaseBlob<256>;
  using AddressTy = ::BaseBlob<160>;

#pragma pack(push, 1)
  struct BlockHeader {
    uint32_t nVersion;
    BaseBlob<256> hashPrevBlock;
    BaseBlob<256> hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;

    void getHash(uint8_t *hash) const {
      CCtxSha256 sha256;
      sha256Init(&sha256);
      sha256Update(&sha256, this, sizeof(*this));
      sha256Final(&sha256, hash);

      sha256Init(&sha256);
      sha256Update(&sha256, hash, 32);
      sha256Final(&sha256, hash);
    }

    BlockHashTy hash() const {
      BaseBlob<256> result;
      getHash(result.begin());
      return result;
    }
  };
#pragma pack(pop)

  struct TxIn {
    BaseBlob<256> previousOutputHash;
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
      BaseBlob<256> result;
      uint8_t buffer[4096];
      xmstream stream(buffer, sizeof(buffer));
      stream.reset();
      BTC::serialize(stream, *this);

      CCtxSha256 sha256;
      sha256Init(&sha256);
      sha256Update(&sha256, stream.data(), stream.sizeOf());
      sha256Final(&sha256, result.begin());
      sha256Init(&sha256);
      sha256Update(&sha256, result.begin(), sizeof(result));
      sha256Final(&sha256, result.begin());
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
  struct CheckConsensusCtx {
    bool HasRtt = false;
    uint32_t PrevBits;
    int64_t PrevHeaderTime[4];

    void initialize(CBlockTemplate&, const std::string&);
    bool hasRtt() { return HasRtt; }
  };

  struct ChainParams {
    UInt<256> powLimit;
  };

  static void checkConsensusInitialize(CheckConsensusCtx&) {}
  static CCheckStatus checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams&, const UInt<256> &shareTarget);
  static CCheckStatus checkConsensus(const Proto::Block &block, CheckConsensusCtx &ctx, ChainParams &params, const UInt<256> &shareTarget) { return checkConsensus(block.header, ctx, params, shareTarget); }
  static double getDifficulty(const Proto::BlockHeader &header) { return BTC::difficultyFromBits(header.nBits, 29); }
  static UInt<256> expectedWork(const Proto::BlockHeader &header, const CheckConsensusCtx&);
  static std::string makeHumanReadableAddress(uint8_t pubkeyAddressPrefix, const BTC::Proto::AddressTy &address);
  static bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, AddressTy &address);
  static bool decodeWIF(const std::string &privateKey, const std::vector<uint8_t> &prefix, uint8_t *result);
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
  inline static const UInt<256> StratumMultiplier = UInt<256>(1u) << 32;

  // TODO: Use this for headers non-compatible with BTC
  struct HeaderBuilder {
    static bool build(Proto::BlockHeader &header, uint32_t *jobVersion, CoinbaseTx &legacy, const std::vector<BaseBlob<256>> &merklePath, rapidjson::Value &blockTemplate);
  };

  struct CoinbaseBuilder {
  public:
    bool prepare(uint64_t *blockReward, rapidjson::Value &blockTemplate);

    void build(int64_t height,
               uint64_t blockReward,
               void *coinbaseData,
               size_t coinbaseSize,
               const std::string &coinbaseMessage,
               const Proto::AddressTy &miningAddress,
               const CMiningConfig &miningCfg,
               bool segwitEnabled,
               const xmstream &witnessCommitment,
               BTC::CoinbaseTx &legacy,
               BTC::CoinbaseTx &witness);

  private:
    uint64_t DevFee = 0;
    uint64_t StakingReward = 0;
    xmstream DevScriptPubKey;
    xmstream StakingRewardScriptPubkey;
  };

  struct Notify {
    static void build(StratumWork *source, typename Proto::BlockHeader &header, uint32_t asicBoostData, CoinbaseTx &legacy, const std::vector<BaseBlob<256>> &merklePath, const CMiningConfig &cfg, bool resetPreviousWork, xmstream &notifyMessage);
  };

  struct Prepare {
    static bool prepare(typename Proto::BlockHeader &header, uint32_t asicBoostData, CoinbaseTx &legacy, CoinbaseTx &witness, const std::vector<BaseBlob<256> > &merklePath, const CWorkerConfig &workerCfg, const CMiningConfig &miningCfg, const CStratumMessage &msg);
  };

  using Work = BTC::WorkTy<BTC::Proto, HeaderBuilder, CoinbaseBuilder, Notify, Prepare>;

  static constexpr bool MergedMiningSupport = false;
  static void buildSendTargetMessage(xmstream &stream, double difficulty) { buildSendTargetMessageImpl(stream, difficulty); }

public:
  static Work *newPrimaryWork(int64_t stratumId,
      PoolBackend *backend,
      size_t backendIdx,
      const CMiningConfig &miningCfg,
      const std::vector<uint8_t> &miningAddress,
      const std::string &coinbaseMessage,
      CBlockTemplate &blockTemplate,
      std::string &error) {
    if (blockTemplate.WorkType != EWorkBitcoin) {
      error = "incompatible work type";
      return nullptr;
    }

    std::unique_ptr<Work> work(new Work(stratumId,
                                        blockTemplate.UniqueWorkId,
                                        backend,
                                        backendIdx,
                                        miningCfg,
                                        miningAddress,
                                        coinbaseMessage));
    return work->loadFromTemplate(blockTemplate, error) ? work.release() : nullptr;
  }
  static StratumSingleWork *newSecondaryWork(int64_t, PoolBackend*, size_t, const CMiningConfig&, const std::vector<uint8_t>&, const std::string&, CBlockTemplate&, std::string&) { return nullptr; }
  static StratumMergedWork *newMergedWork(int64_t, StratumSingleWork*, std::vector<StratumSingleWork*>&, const CMiningConfig&, std::string&) { return nullptr; }

  static EStratumDecodeStatusTy decodeStratumMessage(CStratumMessage &msg, const char *in, size_t size) {
    rapidjson::Document document;
    document.Parse(in, size);
    if (document.HasParseError() || !document.IsObject())
      return EStratumStatusJsonError;

    if (!(document.HasMember("id") && document.HasMember("method") && document.HasMember("params")))
      return EStratumStatusFormatError;

    // Some clients put null to 'params' field
    if (document["params"].IsNull())
      document["params"].SetArray();

    if (!(document["method"].IsString() && document["params"].IsArray()))
      return EStratumStatusFormatError;

    if (document["id"].IsUint64())
      msg.IntegerId = document["id"].GetUint64();
    else if (document["id"].IsString())
      msg.StringId = document["id"].GetString();
    else
      return EStratumStatusFormatError;

    std::string method = document["method"].GetString();
    const rapidjson::Value::Array &params = document["params"].GetArray();
    if (method == "mining.subscribe") {
      msg.Method = ESubscribe;
      if (params.Size() >= 1) {
        if (params[0].IsString())
          msg.Subscribe.minerUserAgent = params[0].GetString();
      }

      if (params.Size() >= 2) {
        if (params[1].IsString())
          msg.Subscribe.sessionId = params[1].GetString();
      }

      if (params.Size() >= 3) {
        if (params[2].IsString())
          msg.Subscribe.connectHost = params[2].GetString();
      }

      if (params.Size() >= 4) {
        if (params[3].IsUint())
          msg.Subscribe.connectPort = params[3].GetUint();
      }
    } else if (method == "mining.authorize" && params.Size() >= 2) {
      msg.Method = EAuthorize;
      if (params[0].IsString() && params[1].IsString()) {
        msg.Authorize.login = params[0].GetString();
        msg.Authorize.password = params[1].GetString();
      } else {
        return EStratumStatusFormatError;
      }
    } else if (method == "mining.extranonce.subscribe") {
      msg.Method = EExtraNonceSubscribe;
    } else if (method == "mining.submit" && params.Size() >= 5) {
      if (params[0].IsString() &&
          params[1].IsString() &&
          params[2].IsString() &&
          params[3].IsString() && params[3].GetStringLength() == 8 &&
          params[4].IsString() && params[4].GetStringLength() == 8) {
        msg.Method = ESubmit;
        msg.Submit.WorkerName = params[0].GetString();
        msg.Submit.JobId = params[1].GetString();
        {
          // extra nonce mutable part
          msg.Submit.BTC.MutableExtraNonce.resize(params[2].GetStringLength() / 2);
          hex2bin(params[2].GetString(), params[2].GetStringLength(), msg.Submit.BTC.MutableExtraNonce.data());
        }
        msg.Submit.BTC.Time = readHexBE<uint32_t>(params[3].GetString(), 4);
        msg.Submit.BTC.Nonce = readHexBE<uint32_t>(params[4].GetString(), 4);

        if (params.Size() >= 6 && params[5].IsString())
          msg.Submit.BTC.VersionBits = readHexBE<uint32_t>(params[5].GetString(), 4);
      } else {
        return EStratumStatusFormatError;
      }
    } else if (method == "mining.multi_version" && params.Size() >= 1) {
      msg.Method = EMultiVersion;
      if (params[0].IsUint()) {
        msg.MultiVersion.Version = params[0].GetUint();
      } else {
        return EStratumStatusFormatError;
      }
    } else if (method == "mining.configure" && params.Size() >= 2) {
      msg.Method = EMiningConfigure;
      msg.MiningConfigure.ExtensionsField = 0;
      // Example:
      // {
      //    "id":1,
      //    "method":"mining.configure",
      //    "params":[
      //       [
      //          "version-rolling"
      //       ],
      //       {
      //          "version-rolling.min-bit-count":2,
      //          "version-rolling.mask":"00c00000"
      //       }
      //    ]
      // }
      if (params[0].IsArray() &&
          params[1].IsObject()) {
        rapidjson::Value::Array extensions = params[0].GetArray();
        rapidjson::Value &arguments = params[1];
        for (rapidjson::SizeType i = 0, ie = extensions.Size(); i != ie; ++i) {
          if (strcmp(extensions[i].GetString(), "version-rolling") == 0) {
            msg.MiningConfigure.ExtensionsField |= StratumMiningConfigure::EVersionRolling;
            if (arguments.HasMember("version-rolling.mask") && arguments["version-rolling.mask"].IsString())
              msg.MiningConfigure.VersionRollingMask = readHexBE<uint32_t>(arguments["version-rolling.mask"].GetString(), 4);
            if (arguments.HasMember("version-rolling.min-bit-count") && arguments["version-rolling.min-bit-count"].IsUint())
              msg.MiningConfigure.VersionRollingMinBitCount = arguments["version-rolling.min-bit-count"].GetUint();
          } else if (strcmp(extensions[i].GetString(), "minimum-difficulty") == 0) {
            if (arguments.HasMember("minimum-difficulty.value")) {
              if (arguments["minimum-difficulty.value"].IsUint64())
                msg.MiningConfigure.MinimumDifficultyValue = static_cast<double>(arguments["minimum-difficulty.value"].GetUint64());
              else if (arguments["minimum-difficulty.value"].IsDouble())
                msg.MiningConfigure.MinimumDifficultyValue = arguments["minimum-difficulty.value"].GetDouble();
            }
            msg.MiningConfigure.ExtensionsField |= StratumMiningConfigure::EMinimumDifficulty;
          } else if (strcmp(extensions[i].GetString(), "subscribe-extranonce") == 0) {
            msg.MiningConfigure.ExtensionsField |= StratumMiningConfigure::ESubscribeExtraNonce;
          }
        }
      } else {
        return EStratumStatusFormatError;
      }
    } else if (method == "mining.suggest_difficulty" && params.Size() >= 1) {
      msg.Method = EMiningSuggestDifficulty;
      if (params[0].IsDouble()) {
        msg.MiningSuggestDifficulty.Difficulty = params[0].GetDouble();
      } else if (params[0].IsUint64()) {
        msg.MiningSuggestDifficulty.Difficulty = static_cast<double>(params[0].GetUint64());
      } else {
        return EStratumStatusFormatError;
      }
    } else {
      return EStratumStatusFormatError;
    }

    return EStratumStatusOk;
  }

  static void miningConfigInitialize(CMiningConfig &miningCfg, rapidjson::Value &instanceCfg) {
    // default values
    miningCfg.FixedExtraNonceSize = 4;
    miningCfg.MutableExtraNonceSize = 4;
    miningCfg.TxNumLimit = 0;

    if (instanceCfg.HasMember("fixedExtraNonceSize") && instanceCfg["fixedExtraNonceSize"].IsUint())
      miningCfg.FixedExtraNonceSize = instanceCfg["fixedExtraNonceSize"].GetUint();
    if (instanceCfg.HasMember("mutableExtraNonceSize") && instanceCfg["mutableExtraNonceSize"].IsUint())
      miningCfg.MutableExtraNonceSize = instanceCfg["mutableExtraNonceSize"].GetUint();
  }

  static void workerConfigInitialize(CWorkerConfig &workerCfg, ThreadConfig &threadCfg) {
    // Set fixed part of extra nonce
    workerCfg.ExtraNonceFixed = threadCfg.ExtraNonceCurrent;

    // Set session names
    uint8_t sessionId[16];
    {
      RAND_bytes(sessionId, sizeof(sessionId));
      workerCfg.SetDifficultySession.resize(sizeof(sessionId)*2);
      bin2hexLowerCase(sessionId, workerCfg.SetDifficultySession.data(), sizeof(sessionId));
    }
    {
      RAND_bytes(sessionId, sizeof(sessionId));
      workerCfg.NotifySession.resize(sizeof(sessionId)*2);
      bin2hexLowerCase(sessionId, workerCfg.NotifySession.data(), sizeof(sessionId));
    }

    // Update thread config
    threadCfg.ExtraNonceCurrent += threadCfg.ThreadsNum;
  }

  static void workerConfigSetupVersionRolling(CWorkerConfig &workerCfg, uint32_t versionMask) {
    workerCfg.AsicBoostEnabled = true;
    workerCfg.VersionMask = versionMask;
  }

  static void workerConfigOnSubscribe(CWorkerConfig &workerCfg, CMiningConfig &miningCfg, CStratumMessage &msg, xmstream &out, std::string &subscribeInfo) {
    // Response format
    // {"id": 1, "result": [ [ ["mining.set_difficulty", <setDifficultySession>:string(hex)], ["mining.notify", <notifySession>:string(hex)]], <uniqueExtraNonce>:string(hex), extraNonceSize:integer], "error": null}\n
    {
      JSON::Object object(out);
      if (!msg.StringId.empty())
        object.addString("id", msg.StringId);
      else
        object.addInt("id", msg.IntegerId);
      object.addField("result");
      {
        JSON::Array result(out);
        result.addField();
        {
          JSON::Array sessions(out);
          sessions.addField();
          {
            JSON::Array setDifficultySession(out);
            setDifficultySession.addString("mining.set_difficulty");
            setDifficultySession.addString(workerCfg.SetDifficultySession);
          }
          sessions.addField();
          {
            JSON::Array notifySession(out);
            notifySession.addString("mining.notify");
            notifySession.addString(workerCfg.NotifySession);
          }
        }

        // Unique extra nonce
        result.addString(writeHexBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize));
        // Mutable part of extra nonce size
        result.addInt(miningCfg.MutableExtraNonceSize);
      }
      object.addNull("error");
    }

    out.write('\n');
    subscribeInfo = std::to_string(workerCfg.ExtraNonceFixed);
  }

  static void buildSendTargetMessageImpl(xmstream &stream, double difficulty) {
    JSON::Object object(stream);
    object.addString("method", "mining.set_difficulty");
    object.addNull("id");
    object.addField("params");
    {
      JSON::Array params(stream);
      params.addDouble(difficulty);
    }
  }

  static UInt<256> targetFromDifficulty(const UInt<256> &difficulty) {
    static const UInt<256> maxTarget = UInt<256>::fromHex("ffff000000000000000000000000000000000000000000000000000000000000");
    return maxTarget / difficulty;
  }
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
