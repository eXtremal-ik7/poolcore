#pragma once

#include "btc.h"
#include "poolcommon/uint.h"
#include <array>

namespace ZEC {
class Proto {
public:
  using BlockHashTy = BTC::Proto::BlockHashTy;
  using TxHashTy = BTC::Proto::TxHashTy;
  using AddressTy = BTC::Proto::AddressTy;

  static constexpr uint32_t OVERWINTER_VERSION_GROUP_ID = 0x03C48270;
  static constexpr uint32_t SAPLING_VERSION_GROUP_ID = 0x892F2085;
  static constexpr int32_t OVERWINTER_TX_VERSION = 3;
  static constexpr int32_t SAPLING_TX_VERSION = 4;

  static constexpr uint8_t G1_PREFIX_MASK = 0x02;
  static constexpr uint8_t G2_PREFIX_MASK = 0x0a;

  static constexpr size_t ZC_NUM_JS_INPUTS = 2;
  static constexpr size_t ZC_NUM_JS_OUTPUTS = 2;
  static constexpr size_t INCREMENTAL_MERKLE_TREE_DEPTH = 29;
  static constexpr size_t INCREMENTAL_MERKLE_TREE_DEPTH_TESTING = 4;
  static constexpr size_t SAPLING_INCREMENTAL_MERKLE_TREE_DEPTH = 32;
  static constexpr size_t NOTEENCRYPTION_AUTH_BYTES = 16;
  static constexpr size_t ZC_NOTEPLAINTEXT_LEADING = 1;
  static constexpr size_t ZC_V_SIZE = 8;
  static constexpr size_t ZC_RHO_SIZE = 32;
  static constexpr size_t ZC_R_SIZE = 32;
  static constexpr size_t ZC_MEMO_SIZE = 512;
  static constexpr size_t ZC_DIVERSIFIER_SIZE = 11;
  static constexpr size_t ZC_JUBJUB_POINT_SIZE = 32;
  static constexpr size_t ZC_JUBJUB_SCALAR_SIZE = 32;
  static constexpr size_t ZC_NOTEPLAINTEXT_SIZE = ZC_NOTEPLAINTEXT_LEADING + ZC_V_SIZE + ZC_RHO_SIZE + ZC_R_SIZE + ZC_MEMO_SIZE;
  static constexpr size_t ZC_SAPLING_ENCPLAINTEXT_SIZE = ZC_NOTEPLAINTEXT_LEADING + ZC_DIVERSIFIER_SIZE + ZC_V_SIZE + ZC_R_SIZE + ZC_MEMO_SIZE;
  static constexpr size_t ZC_SAPLING_OUTPLAINTEXT_SIZE = ZC_JUBJUB_POINT_SIZE + ZC_JUBJUB_SCALAR_SIZE;
  static constexpr size_t ZC_SAPLING_ENCCIPHERTEXT_SIZE = ZC_SAPLING_ENCPLAINTEXT_SIZE + NOTEENCRYPTION_AUTH_BYTES;
  static constexpr size_t ZC_SAPLING_OUTCIPHERTEXT_SIZE = ZC_SAPLING_OUTPLAINTEXT_SIZE + NOTEENCRYPTION_AUTH_BYTES;

  static constexpr size_t GROTH_PROOF_SIZE = (
      48 + // π_A
      96 + // π_B
      48); // π_C

  template<size_t MLEN>
  struct NoteEncryption {
      enum { CLEN=MLEN+NOTEENCRYPTION_AUTH_BYTES };
      uint256 epk;
      uint256 esk;
      unsigned char nonce;
      uint256 hSig;
  };

  using ZCNoteEncryption = NoteEncryption<ZC_NOTEPLAINTEXT_SIZE>;

#pragma pack(push, 1)
  struct BlockHeader {
  public:
    static constexpr size_t HEADER_SIZE = 4+32+32+32+4+4+32;

  public:
    int32_t nVersion;
    BaseBlob<256> hashPrevBlock;
    BaseBlob<256> hashMerkleRoot;
    BaseBlob<256> hashLightClientRoot;
    uint32_t nTime;
    uint32_t nBits;
    BaseBlob<256> nNonce;
    xvector<uint8_t> nSolution;

    void getHash(uint8_t *result) const {
      uint8_t buffer[2048];
      xmstream localStream(buffer, sizeof(buffer));
      localStream.reset();
      BTC::serialize(localStream, nSolution);

      CCtxSha256 sha256;
      sha256Init(&sha256);
      sha256Update(&sha256, this, HEADER_SIZE);
      sha256Update(&sha256, localStream.data(), localStream.sizeOf());
      sha256Final(&sha256, result);

      sha256Init(&sha256);
      sha256Update(&sha256, result, 32);
      sha256Final(&sha256, result);
    }

    BaseBlob<256> hash() const {
      BaseBlob<256> result;
      getHash(result.begin());
      return result;
    }
  };
#pragma pack(pop)

  using TxIn = BTC::Proto::TxIn;
  using TxOut = BTC::Proto::TxOut;

  struct SpendDescription {
    BaseBlob<256> cv;
    BaseBlob<256> anchor;
    BaseBlob<256> nullifer;
    BaseBlob<256> rk;
    std::array<uint8_t, GROTH_PROOF_SIZE> zkproof;
    std::array<uint8_t, 64> spendAuthSig;
  };

  struct OutputDescription {
    BaseBlob<256> cv;
    BaseBlob<256> cmu;
    BaseBlob<256> ephemeralKey;
    std::array<uint8_t, ZC_SAPLING_ENCCIPHERTEXT_SIZE> encCiphertext;
    std::array<uint8_t, ZC_SAPLING_OUTCIPHERTEXT_SIZE> outCiphertext;
    std::array<uint8_t, GROTH_PROOF_SIZE> zkproof;
  };

  struct CompressedG1 {
    bool y_lsb;
    base_blob<256> x;
  };

  struct CompressedG2 {
    bool y_gt;
    base_blob<512> x;
  };

  struct PHGRProof {
    CompressedG1 g_A;
    CompressedG1 g_A_prime;
    CompressedG2 g_B;
    CompressedG1 g_B_prime;
    CompressedG1 g_C;
    CompressedG1 g_C_prime;
    CompressedG1 g_K;
    CompressedG1 g_H;
  };

  struct JSDescription {
    int64_t vpub_old;
    int64_t vpub_new;
    BaseBlob<256> anchor;
    BaseBlob<256> nullifier1;
    BaseBlob<256> nullifier2;
    BaseBlob<256> commitment1;
    BaseBlob<256> commitment2;
    BaseBlob<256> ephemeralKey;
    std::array<uint8_t, ZCNoteEncryption::CLEN> ciphertext1;
    std::array<uint8_t, ZCNoteEncryption::CLEN> ciphertext2;
    BaseBlob<256> randomSeed;
    BaseBlob<256> mac1;
    BaseBlob<256> mac2;

    PHGRProof phgrProof;
    std::array<uint8_t, GROTH_PROOF_SIZE> zkproof;
  };

  struct Transaction {
    bool fOverwintered;
    int32_t version;
    uint32_t nVersionGroupId;
    xvector<TxIn> txIn;
    xvector<TxOut> txOut;
    uint32_t lockTime;
    uint32_t nExpiryHeight;
    int64_t valueBalance;
    xvector<SpendDescription> vShieldedSpend;
    xvector<OutputDescription> vShieldedOutput;
    xvector<JSDescription> vJoinSplit;
    std::array<uint8_t, 32> joinSplitPubKey;
    std::array<uint8_t, 64> joinSplitSig;
    std::array<uint8_t, 64> bindingSig;

    // Memory only
    uint32_t SerializedDataOffset = 0;
    uint32_t SerializedDataSize = 0;

    BlockHashTy getTxId() const;
  };

  using Block = BTC::Proto::BlockTy<ZEC::Proto>;
  struct CheckConsensusCtx {
  public:
    void initialize(CBlockTemplate&, const std::string &ticker) {
      if (ticker.find(".regtest") != ticker.npos) {
        N = 48;
        K = 5;
      } else {
        N = 200;
        K = 9;
      }
    }

    bool hasRtt() { return false; }

  public:
    unsigned N = 0;
    unsigned K = 0;
  };

  using ChainParams = BTC::Proto::ChainParams;

  static void checkConsensusInitialize(CheckConsensusCtx&) {}
  static CCheckStatus checkConsensus(const ZEC::Proto::BlockHeader &header, CheckConsensusCtx&consensusCtx, ZEC::Proto::ChainParams&);
  static CCheckStatus checkConsensus(const ZEC::Proto::Block &block, CheckConsensusCtx &ctx, ZEC::Proto::ChainParams &chainParams) { return checkConsensus(block.header, ctx, chainParams); }
  static double getDifficulty(const ZEC::Proto::BlockHeader &header) { return BTC::difficultyFromBits(header.nBits, 32); }
  static double expectedWork(const ZEC::Proto::BlockHeader &header, const CheckConsensusCtx&) { return getDifficulty(header); }
  static bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, AddressTy &address) { return BTC::Proto::decodeHumanReadableAddress(hrAddress, pubkeyAddressPrefix, address); }
};

class Stratum {
public:
  static constexpr double DifficultyFactor = 1.0;

  static EStratumDecodeStatusTy decodeStratumMessage(CStratumMessage &msg, const char *in, size_t size) {
    rapidjson::Document document;
    document.Parse(in, size);
    if (document.HasParseError()) {
      return EStratumStatusJsonError;
    }

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
          params[2].IsString() && params[2].GetStringLength() == 8 &&
          params[3].IsString() &&
          params[4].IsString()) {
        msg.Method = ESubmit;
        msg.Submit.WorkerName = params[0].GetString();
        msg.Submit.JobId = params[1].GetString();
        msg.Submit.ZEC.Time = readHexBE<uint32_t>(params[2].GetString(), 4);
        msg.Submit.ZEC.Nonce = params[3].GetString();
        msg.Submit.ZEC.Solution = params[4].GetString();
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

  static void miningConfigInitialize(CMiningConfig &miningCfg, rapidjson::Value &instanceCfg) { BTC::Stratum::miningConfigInitialize(miningCfg, instanceCfg); }

  static void workerConfigInitialize(CWorkerConfig &workerCfg, ThreadConfig &threadCfg) {
    // Set fixed part of extra nonce
    workerCfg.ExtraNonceFixed = threadCfg.ExtraNonceCurrent;

    // Set session names
    uint8_t sessionId[16];
    {
      RAND_bytes(sessionId, sizeof(sessionId));
      workerCfg.NotifySession.resize(sizeof(sessionId)*2);
      bin2hexLowerCase(sessionId, workerCfg.NotifySession.data(), sizeof(sessionId));
    }

    // Update thread config
    threadCfg.ExtraNonceCurrent += threadCfg.ThreadsNum;
  }

  static void workerConfigSetupVersionRolling(CWorkerConfig&, uint32_t) {}

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
        result.addString(workerCfg.NotifySession);
        // Unique extra nonce
        result.addString(writeHexBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize));
      }

      object.addNull("error");
    }

    out.write('\n');
    subscribeInfo = std::to_string(workerCfg.ExtraNonceFixed);
  }

  using CWork = StratumWork;

  struct HeaderBuilder {
    static bool build(Proto::BlockHeader &header, uint32_t *jobVersion, BTC::CoinbaseTx &legacy, const std::vector<BaseBlob<256> > &merklePath, rapidjson::Value &blockTemplate);
  };

  struct CoinbaseBuilder {
  public:
    bool prepare(int64_t *blockReward, rapidjson::Value &blockTemplate);

    void build(int64_t height,
               int64_t blockReward,
               void *coinbaseData,
               size_t coinbaseSize,
               const std::string &coinbaseMessage,
               const Proto::AddressTy &miningAddress,
               const CMiningConfig&,
               bool,
               const xmstream&,
               BTC::CoinbaseTx &legacy,
               BTC::CoinbaseTx &witness);

  private:
    Proto::Transaction CoinbaseTx;
  };

  struct Notify {
    static void build(CWork *source, typename Proto::BlockHeader &header, uint32_t, BTC::CoinbaseTx&, const std::vector<BaseBlob<256> > &, const CMiningConfig&, bool resetPreviousWork, xmstream &notifyMessage);
  };

  struct Prepare {
    static bool prepare(Proto::BlockHeader &header, uint32_t, BTC::CoinbaseTx &legacy, BTC::CoinbaseTx &, const std::vector<BaseBlob<256> > &, const CWorkerConfig &workerCfg, const CMiningConfig &miningCfg, const CStratumMessage &msg);
  };

  using Work = BTC::WorkTy<ZEC::Proto, HeaderBuilder, CoinbaseBuilder, ZEC::Stratum::Notify, ZEC::Stratum::Prepare>;

  static constexpr bool MergedMiningSupport = false;

  static Work *newPrimaryWork(int64_t stratumId,
      PoolBackend *backend,
      size_t backendIdx,
      const CMiningConfig &miningCfg,
      const std::vector<uint8_t> &miningAddress,
      const std::string &coinbaseMessage,
      CBlockTemplate &blockTemplate,
      std::string &error) {
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

  static void buildSendTargetMessage(xmstream &stream, double difficulty);
};

struct X {
  using Proto = ZEC::Proto;
  using Stratum = ZEC::Stratum;
  template<typename T> static inline void serialize(xmstream &src, const T &data) { BTC::Io<T>::serialize(src, data); }
  template<typename T> static inline void unserialize(xmstream &dst, T &data) { BTC::Io<T>::unserialize(dst, data); }
};
}

namespace BTC {
template<> struct Io<ZEC::Proto::BlockHeader> {
  static void serialize(xmstream &dst, const ZEC::Proto::BlockHeader &data);
};

template<> struct Io<ZEC::Proto::CompressedG1> {
  static void serialize(xmstream &dst, const ZEC::Proto::CompressedG1 &data);
  static void unserialize(xmstream &src, ZEC::Proto::CompressedG1 &data);
};

template<> struct Io<ZEC::Proto::CompressedG2> {
  static void serialize(xmstream &dst, const ZEC::Proto::CompressedG2 &data);
  static void unserialize(xmstream &src, ZEC::Proto::CompressedG2 &data);
};

template<> struct Io<ZEC::Proto::SpendDescription> {
  static void serialize(xmstream &dst, const ZEC::Proto::SpendDescription &data);
  static void unserialize(xmstream &src, ZEC::Proto::SpendDescription &data);
};

template<> struct Io<ZEC::Proto::OutputDescription> {
  static void serialize(xmstream &dst, const ZEC::Proto::OutputDescription &data);
  static void unserialize(xmstream &src, ZEC::Proto::OutputDescription &data);
};

template<> struct Io<ZEC::Proto::PHGRProof> {
  static void serialize(xmstream &dst, const ZEC::Proto::PHGRProof &data);
  static void unserialize(xmstream &src, ZEC::Proto::PHGRProof &data);
};

template<> struct Io<ZEC::Proto::JSDescription, bool> {
  static void serialize(xmstream &dst, const ZEC::Proto::JSDescription &data, bool useGroth);
  static void unserialize(xmstream &src, ZEC::Proto::JSDescription &data, bool useGroth);
};

template<> struct Io<ZEC::Proto::Transaction> {
  static void serialize(xmstream &dst, const ZEC::Proto::Transaction &data);
  static void unserialize(xmstream &src, ZEC::Proto::Transaction &data);
};
}
