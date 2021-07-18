#pragma once
#include "merkleTree.h"
#include "poolinstances/stratumMsg.h"
#include "poolcommon/jsonSerializer.h"
#include "stratumWork.h"
#include "serialize.h"
#include "loguru.hpp"
#include <openssl/rand.h>
#include <unordered_map>

namespace BTC {
namespace Script {
  enum {
    OP_0 = 0,
    OP_RETURN = 0x6A,
    OP_DUP = 0x76,
    OP_EQUAL = 0x87,
    OP_EQUALVERIFY = 0x88,
    OP_HASH160 = 0xA9,
    OP_CHECKSIG = 0xAC
  };
}

struct StratumMessage {
  int64_t IntegerId;
  std::string StringId;
  EStratumMethodTy Method;

  StratumMiningSubscribe Subscribe;
  StratumAuthorize Authorize;
  StratumSubmit Submit;
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

struct MiningConfig {
  unsigned FixedExtraNonceSize = 4;
  unsigned MutableExtraNonceSize = 4;
  unsigned TxNumLimit = 0;

  void initialize(rapidjson::Value &instanceCfg) {
    if (instanceCfg.HasMember("fixedExtraNonceSize") && instanceCfg["fixedExtraNonceSize"].IsUint())
      FixedExtraNonceSize = instanceCfg["fixedExtraNonceSize"].GetUint();
    if (instanceCfg.HasMember("mutableExtraNonceSize") && instanceCfg["mutableExtraNonceSize"].IsUint())
      MutableExtraNonceSize = instanceCfg["mutableExtraNonceSize"].GetUint();
  }
};

struct CWorkerConfig {
  std::string SetDifficultySession;
  std::string NotifySession;
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
      SetDifficultySession.resize(sizeof(sessionId)*2);
      bin2hexLowerCase(sessionId, SetDifficultySession.data(), sizeof(sessionId));
    }
    {
      RAND_bytes(sessionId, sizeof(sessionId));
      NotifySession.resize(sizeof(sessionId)*2);
      bin2hexLowerCase(sessionId, NotifySession.data(), sizeof(sessionId));
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
        result.addField();
        {
          JSON::Array sessions(out);
          sessions.addField();
          {
            JSON::Array setDifficultySession(out);
            setDifficultySession.addString("mining.set_difficulty");
            setDifficultySession.addString(SetDifficultySession);
          }
          sessions.addField();
          {
            JSON::Array notifySession(out);
            notifySession.addString("mining.notify");
            notifySession.addString(NotifySession);
          }
        }

        // Unique extra nonce
        result.addString(writeHexBE(ExtraNonceFixed, miningCfg.FixedExtraNonceSize));
        // Mutable part of extra nonce size
        result.addInt(miningCfg.MutableExtraNonceSize);
      }
      object.addNull("error");
    }

    out.write('\n');
    subscribeInfo = std::to_string(ExtraNonceFixed);
  }

};

static inline double getDifficulty(uint32_t bits)
{
    int nShift = (bits >> 24) & 0xff;
    double dDiff =
        (double)0x0000ffff / (double)(bits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

struct CoinbaseTx {
  xmstream Data;
  unsigned ExtraDataOffset;
  unsigned ExtraNonceOffset;
};

struct TxData {
  const char *HexData;
  size_t HexDataSize;
  uint256 TxId;
  uint256 WitnessHash;
};

struct TxTree {
  TxData Data;
  int64_t Fee;
  size_t DependsOn = std::numeric_limits<size_t>::max();
  bool Visited = false;
};

bool addTransaction(TxTree *tree, size_t index, size_t txNumLimit, std::vector<TxData> &result, int64_t *blockReward);
bool transactionChecker(rapidjson::Value::Array transactions, std::vector<TxData> &result);
bool isSegwitEnabled(rapidjson::Value::Array transactions);
bool calculateWitnessCommitment(rapidjson::Value &blockTemplate, bool txFilter, std::vector<TxData> &processedTransactions, xmstream &witnessCommitment, std::string &error);
void collectTransactions(const std::vector<TxData> &processedTransactions, xmstream &txHexData, std::vector<uint256> &merklePath, size_t &txNum);

template<typename Proto>
bool transactionFilter(rapidjson::Value::Array transactions, size_t txNumLimit, std::vector<TxData> &result, int64_t *blockReward, bool sortByHash)
{
  size_t txNum = transactions.Size();
  std::unique_ptr<TxTree[]> txTree(new TxTree[txNum]);

  // Build hashmap txid -> index
  std::unordered_map<uint256, size_t> txidMap;
  for (size_t i = 0; i < txNum; i++) {
    rapidjson::Value &txSrc = transactions[i];

    if (!txSrc.HasMember("data") || !txSrc["data"].IsString())
      return false;
    txTree[i].Data.HexData = txSrc["data"].GetString();
    txTree[i].Data.HexDataSize = txSrc["data"].GetStringLength();

    if (txSrc.HasMember("txid") && txSrc["txid"].IsString()) {
      txTree[i].Data.TxId.SetHex(txSrc["txid"].GetString());
      if (txSrc.HasMember("hash"))
        txTree[i].Data.WitnessHash.SetHex(txSrc["hash"].GetString());
    } else if (txSrc.HasMember("hash") && txSrc["hash"].IsString()) {
      txTree[i].Data.TxId.SetHex(txSrc["hash"].GetString());
    } else {
      return false;
    }

    if (!txSrc.HasMember("fee") || !txSrc["fee"].IsInt64())
      return false;
    txTree[i].Fee = txSrc["fee"].GetInt64();

    txidMap[txTree[i].Data.TxId] = i;
    *blockReward -= txTree[i].Fee;
  }

  xmstream txBinaryData;
  typename Proto::Transaction tx;
  for (size_t i = 0; i < txNum; i++) {
    rapidjson::Value &txSrc = transactions[i];
    if (!txSrc.HasMember("data") || !txSrc["data"].IsString())
      return false;

    // Convert hex -> binary data
    txBinaryData.reset();
    const char *txHexData = txSrc["data"].GetString();
    size_t txHexSize = txSrc["data"].GetStringLength();
    hex2bin(txHexData, txHexSize, txBinaryData.reserve<uint8_t>(txHexSize/2));

    // Decode BTC transaction
    txBinaryData.seekSet(0);
    BTC::unserialize(txBinaryData, tx);
    if (txBinaryData.eof() || txBinaryData.remaining())
      return false;

    // Iterate txin, found in-block dependencies
    for (const auto &txin: tx.txIn) {
      auto It = txidMap.find(txin.previousOutputHash);
      if (It != txidMap.end())
        txTree[i].DependsOn = It->second;
    }
  }

  for (size_t i = 0; i < txNum; i++) {
    // Add transactions with its dependencies recursively
    if (!addTransaction(txTree.get(), i, txNumLimit, result, blockReward))
      break;
  }

  // TODO: sort by hash (for BCHN, BCHABC)
  if (sortByHash)
    std::sort(result.begin(), result.end(), [](const TxData &l, const TxData &r) {
      // TODO: use binary representation of txid
      return l.TxId.GetHex() < r.TxId.GetHex();
    });

  return true;
}

template<typename Proto, typename HeaderBuilderTy, typename CoinbaseBuilderTy, typename NotifyTy, typename PrepareForSubmitTy, typename MiningConfigTy, typename WorkerConfigTy, typename StratumMessageTy>
class WorkTy : public StratumSingleWork<typename Proto::BlockHashTy, MiningConfigTy, WorkerConfigTy, StratumMessageTy> {
public:
  WorkTy(int64_t stratumWorkId, uint64_t uniqueWorkId, PoolBackend *backend, size_t backendIdx, const MiningConfigTy &miningCfg, const std::vector<uint8_t> &miningAddress, const std::string &coinbaseMessage) :
    StratumSingleWork<typename Proto::BlockHashTy, MiningConfigTy, WorkerConfigTy, StratumMessageTy>(stratumWorkId, uniqueWorkId, backend, backendIdx, miningCfg) {
    CoinbaseMessage_ = coinbaseMessage;
    this->Initialized_ = miningAddress.size() == sizeof(typename Proto::AddressTy);
    if (this->Initialized_)
      memcpy(MiningAddress_.begin(), &miningAddress[0], miningAddress.size());
  }
  virtual typename Proto::BlockHashTy shareHash() override { return Header.GetHash(); }
  virtual std::string blockHash(size_t) override { return Header.GetHash().ToString(); }
  virtual double expectedWork(size_t) override { return getDifficulty(Header.nBits); }
  virtual bool ready() override { return this->Backend_ != nullptr; }

  virtual void buildBlock(size_t, xmstream &blockHexData) override { buildBlockImpl(Header, CBTxWitness_, blockHexData); }

  virtual void mutate() override {
    Header.nTime = static_cast<uint32_t>(time(nullptr));
    buildNotifyMessageImpl(this, Header, JobVersion, CBTxLegacy_, MerklePath, this->MiningCfg_, true, this->NotifyMessage_);
  }

  virtual bool checkConsensus(size_t, double *shareDiff) override { return checkConsensusImpl(Header, ConsensusCtx_, shareDiff); }

  virtual void buildNotifyMessage(bool resetPreviousWork) override {
    buildNotifyMessageImpl(this, Header, JobVersion, CBTxLegacy_, MerklePath, this->MiningCfg_, resetPreviousWork, this->NotifyMessage_);
  }

  virtual bool prepareForSubmit(const WorkerConfigTy &workerCfg, const StratumMessageTy &msg) override {
    return prepareForSubmitImpl(Header, JobVersion, CBTxLegacy_, CBTxWitness_, MerklePath, workerCfg, this->MiningCfg_, msg);
  }

  virtual bool loadFromTemplate(CBlockTemplate &blockTemplate, const std::string &ticker, std::string &error) override {
    if (!blockTemplate.Document.HasMember("result") || !blockTemplate.Document["result"].IsObject()) {
      error = "no result";
      return false;
    }

    rapidjson::Value &resultValue = blockTemplate.Document["result"];

    // Check fields:
    // height
    // transactions
    if (!resultValue.HasMember("height") ||
        !resultValue.HasMember("transactions") || !resultValue["transactions"].IsArray()) {
      error = "missing data";
      return false;
    }

    rapidjson::Value &height = resultValue["height"];
    rapidjson::Value::Array transactions = resultValue["transactions"].GetArray();
    if (!height.IsUint64()) {
      error = "missing height";
      return false;
    }

    this->Height_ = height.GetUint64();

    // Check segwit enabled (compare txid and hash for all transactions)
    SegwitEnabled = isSegwitEnabled(transactions);

    // Checking/filtering transactions
    int64_t blockRewardDelta = 0;
    bool txFilter = this->MiningCfg_.TxNumLimit && transactions.Size() > this->MiningCfg_.TxNumLimit;
    std::vector<TxData> processedTransactions;
    bool needSortByHash = (ticker == "BCHN" || ticker == "BCHABC");

    bool transactionCheckResult;
    if (txFilter)
      transactionCheckResult = transactionFilter<Proto>(transactions, this->MiningCfg_.TxNumLimit, processedTransactions, &blockRewardDelta, needSortByHash);
    else
      transactionCheckResult = transactionChecker(transactions, processedTransactions);
    if (!transactionCheckResult) {
      error = "template contains invalid transactions";
      return false;
    }

    CoinbaseBuilder_.prepare(&this->BlockReward_, resultValue);

    this->BlockReward_ -= blockRewardDelta;

    if (txFilter)
      LOG_F(INFO, " * [txfilter] transactions num %zu -> %zu; coinbase value %" PRIi64 " -> %" PRIi64 "", static_cast<size_t>(transactions.Size()), processedTransactions.size(), this->BlockReward_+blockRewardDelta, this->BlockReward_);

    // Calculate witness commitment
    if (SegwitEnabled) {
      if (!calculateWitnessCommitment(resultValue, txFilter, processedTransactions, WitnessCommitment, error))
        return false;
    }

    // Coinbase
    buildCoinbaseTx(nullptr, 0, this->MiningCfg_, CBTxLegacy_, CBTxWitness_);

    // Transactions
    collectTransactions(processedTransactions, TxHexData, MerklePath, this->TxNum_);

    // Fill header
    if (!HeaderBuilderTy::build(Header, &JobVersion, CBTxLegacy_, MerklePath, resultValue)) {
      error = "missing header data";
      return false;
    }

    ConsensusCtx_.initialize(ticker);
    return true;
  }

  virtual double getAbstractProfitValue(size_t, double price, double coeff) override {
    return price * this->BlockReward_ / getDifficulty(Header.nBits) * coeff;
  }

public:
  // Implementation
  /// Build & serialize custom coinbase transaction
  void buildCoinbaseTx(void *coinbaseData, size_t coinbaseSize, const MiningConfig &miningCfg, CoinbaseTx &legacy, CoinbaseTx &witness) {
    CoinbaseBuilder_.build(this->Height_, this->BlockReward_, coinbaseData, coinbaseSize, this->CoinbaseMessage_, this->MiningAddress_, miningCfg, SegwitEnabled, WitnessCommitment, legacy, witness);
  }

  static bool checkConsensusImpl(const typename Proto::BlockHeader &header, typename Proto::CheckConsensusCtx &consensusCtx, double *shareDiff) {
    typename Proto::ChainParams params;
    return Proto::checkConsensus(header, consensusCtx, params, shareDiff);
  }

  static void buildNotifyMessageImpl(StratumWork<typename Proto::BlockHashTy, MiningConfigTy, WorkerConfigTy, StratumMessageTy> *source, typename Proto::BlockHeader &header, uint32_t asicBoostData, CoinbaseTx &legacy, const std::vector<uint256> &merklePath, const MiningConfig &cfg, bool resetPreviousWork, xmstream &notifyMessage) {
    NotifyTy::build(source, header, asicBoostData, legacy, merklePath, cfg, resetPreviousWork, notifyMessage);
  }

  static bool prepareForSubmitImpl(typename Proto::BlockHeader &header, uint32_t asicBoostData, CoinbaseTx &legacy, CoinbaseTx &witness, const std::vector<uint256> &merklePath, const WorkerConfigTy &workerCfg, const MiningConfigTy &miningCfg, const StratumMessageTy &msg) {
    return PrepareForSubmitTy::prepare(header, asicBoostData, legacy, witness, merklePath, workerCfg, miningCfg, msg);
  }

  void buildBlockImpl(typename Proto::BlockHeader &header, CoinbaseTx &witness, xmstream &blockHexData) {
    blockHexData.reset();
    {
      // Header
      uint8_t buffer[1024];
      xmstream stream(buffer, sizeof(buffer));
      stream.reset();
      BTC::serialize(stream, header);

      // Transactions count
      BTC::serializeVarSize(stream, this->TxNum_ + 1);
      bin2hexLowerCase(stream.data(), blockHexData.reserve<char>(stream.sizeOf()*2), stream.sizeOf());
    }

    // Coinbase (witness)
    bin2hexLowerCase(witness.Data.data(), blockHexData.reserve<char>(witness.Data.sizeOf()*2), witness.Data.sizeOf());

    // Transactions
    blockHexData.write(TxHexData.data(), TxHexData.sizeOf());
  }

public:
  // Header
  typename Proto::BlockHeader Header;
  // ASIC boost data
  uint32_t JobVersion;
  // Various block template data
  bool SegwitEnabled = false;
  std::vector<uint256> MerklePath;
  // Coinbase data
  typename Proto::AddressTy MiningAddress_;
  std::string CoinbaseMessage_;
  CoinbaseBuilderTy CoinbaseBuilder_;
  xmstream WitnessCommitment;
  CoinbaseTx CBTxLegacy_;
  CoinbaseTx CBTxWitness_;
  // Transaction data
  xmstream TxHexData;
  // PoW check context
  typename Proto::CheckConsensusCtx ConsensusCtx_;
};

}
