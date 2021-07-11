#pragma once

#include "btc.h"

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
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint256 hashLightClientRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint256 nNonce;
    xvector<uint8_t> nSolution;

    BlockHashTy GetHash() const {
      uint8_t buffer[2048];
      uint256 result;
      xmstream localStream(buffer, sizeof(buffer));
      localStream.reset();
      BTC::serialize(localStream, nSolution);

      SHA256_CTX sha256;
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, this, HEADER_SIZE);
      SHA256_Update(&sha256, localStream.data(), localStream.sizeOf());
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

  struct SpendDescription {
    uint256 cv;
    uint256 anchor;
    uint256 nullifer;
    uint256 rk;
    std::array<uint8_t, GROTH_PROOF_SIZE> zkproof;
    std::array<uint8_t, 64> spendAuthSig;
  };

  struct OutputDescription {
    uint256 cv;
    uint256 cmu;
    uint256 ephemeralKey;
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
    uint256 anchor;
    uint256 nullifier1;
    uint256 nullifier2;
    uint256 commitment1;
    uint256 commitment2;
    uint256 ephemeralKey;
    std::array<uint8_t, ZCNoteEncryption::CLEN> ciphertext1;
    std::array<uint8_t, ZCNoteEncryption::CLEN> ciphertext2;
    uint256 randomSeed;
    uint256 mac1;
    uint256 mac2;

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
    void initialize(const std::string &ticker) {
      if (ticker.find(".regtest") != ticker.npos) {
        N = 48;
        K = 5;
      } else {
        N = 200;
        K = 9;
      }
    }

  public:
    unsigned N = 0;
    unsigned K = 0;
  };

  using ChainParams = BTC::Proto::ChainParams;

  static void checkConsensusInitialize(CheckConsensusCtx&) {}
  static bool checkConsensus(const ZEC::Proto::BlockHeader&header, CheckConsensusCtx&consensusCtx, ZEC::Proto::ChainParams&, double *shareDiff);
  static bool checkConsensus(const ZEC::Proto::Block &block, CheckConsensusCtx &ctx, ZEC::Proto::ChainParams &chainParams, double *shareDiff) { return checkConsensus(block.header, ctx, chainParams, shareDiff); }
  static bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, AddressTy &address) { return BTC::Proto::decodeHumanReadableAddress(hrAddress, pubkeyAddressPrefix, address); }
};

class Stratum {
public:
  static constexpr double DifficultyFactor = 1.0;

  struct ZECStratumSubmit {
    std::string WorkerName;
    std::string JobId;
    uint32_t Time;
    std::string Nonce;
    std::string Solution;

    // TODO: remove
    std::optional<uint32_t> VersionBits;
  };

  struct StratumMessage {
    int64_t IntegerId;
    std::string StringId;
    EStratumMethodTy Method;

    StratumMiningSubscribe Subscribe;
    StratumAuthorize Authorize;
    ZECStratumSubmit Submit;
    StratumMultiVersion MultiVersion;
    StratumMiningConfigure MiningConfigure;
    StratumMiningSuggestDifficulty MiningSuggestDifficulty;

    std::string Error;

    EStratumDecodeStatusTy decodeStratumMessage(const char *in, size_t size);
    void addId(JSON::Object &object) {
      if (!StringId.empty())
        object.addString("id", StringId);
      else
        object.addInt("id", IntegerId);
    }
  };

  using MiningConfig = BTC::MiningConfig;

  struct WorkerConfig {
    std::string Session;
    uint64_t ExtraNonceFixed;
    bool AsicBoostEnabled = false;
    uint32_t VersionMask = 0;

    static inline void addId(JSON::Object &object, StratumMessage &msg) {
      if (!msg.StringId.empty())
        object.addString("id", msg.StringId);
      else
        object.addInt("id", msg.IntegerId);
    }

    void initialize(ThreadConfig &threadCfg) {
      // Set fixed part of extra nonce
      ExtraNonceFixed = threadCfg.ExtraNonceCurrent;

      // Set session names
      uint8_t sessionId[16];
      {
        RAND_bytes(sessionId, sizeof(sessionId));
        Session.resize(sizeof(sessionId)*2);
        bin2hexLowerCase(sessionId, Session.data(), sizeof(sessionId));
      }

      // Update thread config
      threadCfg.ExtraNonceCurrent += threadCfg.ThreadsNum;
    }

    void setupVersionRolling(uint32_t versionMask) {
      AsicBoostEnabled = true;
      VersionMask = versionMask;
    }

    void onSubscribe(BTC::MiningConfig &miningCfg, StratumMessage &msg, xmstream &out, std::string &subscribeInfo) {
      // Response format
      // {"id": 1, "result": [ [ ["mining.set_difficulty", <setDifficultySession>:string(hex)], ["mining.notify", <notifySession>:string(hex)]], <uniqueExtraNonce>:string(hex), extraNonceSize:integer], "error": null}\n
      {
        JSON::Object object(out);
        addId(object, msg);
        object.addField("result");

        {
          JSON::Array result(out);
          result.addString(Session);
          // Unique extra nonce
          result.addString(writeHexBE(ExtraNonceFixed, miningCfg.FixedExtraNonceSize));
        }

        object.addNull("error");
      }

      out.write('\n');
      subscribeInfo = std::to_string(ExtraNonceFixed);
    }

  };

  using CWork = StratumWork<Proto::BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>;

  struct HeaderBuilder {
    static bool build(Proto::BlockHeader &header, uint32_t *jobVersion, BTC::CoinbaseTx &legacy, const std::vector<uint256> &merklePath, rapidjson::Value &blockTemplate);
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
               const MiningConfig&,
               bool,
               const xmstream&,
               BTC::CoinbaseTx &legacy,
               BTC::CoinbaseTx &witness);

  private:
    Proto::Transaction CoinbaseTx;
  };

  struct Notify {
    static void build(CWork *source, typename Proto::BlockHeader &header, uint32_t, BTC::CoinbaseTx&, const std::vector<uint256>&, const MiningConfig&, bool resetPreviousWork, xmstream &notifyMessage);
  };

  struct Prepare {
    static bool prepare(Proto::BlockHeader &header, uint32_t, BTC::CoinbaseTx &legacy, BTC::CoinbaseTx &, const std::vector<uint256> &, const WorkerConfig &workerCfg, const MiningConfig &miningCfg, const StratumMessage &msg);
  };

  using Work = BTC::WorkTy<ZEC::Proto, HeaderBuilder, CoinbaseBuilder, ZEC::Stratum::Notify, ZEC::Stratum::Prepare, MiningConfig, WorkerConfig, StratumMessage>;
  using SecondWork = StratumSingleWorkEmpty<Proto::BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>;
  using MergedWork = StratumMergedWorkEmpty<Proto::BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>;

  static constexpr bool MergedMiningSupport = false;
  static bool isMainBackend(const std::string&) { return true; }
  static bool keepOldWorkForBackend(const std::string&) { return false; }
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
