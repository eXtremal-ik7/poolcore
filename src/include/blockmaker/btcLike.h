#pragma once
#include "poolcommon/utils.h"
#include "poolinstances/stratumMsg.h"

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

struct CoinbaseTx {
  xmstream Data;
  unsigned ExtraDataOffset;
  unsigned ExtraNonceOffset;
};

struct TxData {
  const char *HexData;
  size_t HexDataSize;
  BaseBlob<256> TxId;
  BaseBlob<256> WitnessHash;
};

struct TxTree {
  TxData Data;
  uint64_t Fee;
  size_t DependsOn = std::numeric_limits<size_t>::max();
  bool Visited = false;
};

bool addTransaction(TxTree *tree, size_t index, size_t txNumLimit, std::vector<TxData> &result, uint64_t *blockReward);
bool transactionChecker(rapidjson::Value::Array transactions, std::vector<TxData> &result);
bool isSegwitEnabled(rapidjson::Value::Array transactions);
bool calculateWitnessCommitment(rapidjson::Value &blockTemplate, bool txFilter, std::vector<TxData> &processedTransactions, xmstream &witnessCommitment, std::string &error);
void collectTransactions(const std::vector<TxData> &processedTransactions, xmstream &txHexData, std::vector<BaseBlob<256> > &merklePath, size_t &txNum);

template<typename Proto>
bool transactionFilter(rapidjson::Value::Array transactions, size_t txNumLimit, std::vector<TxData> &result, uint64_t *blockReward, bool sortByHash)
{
  size_t txNum = transactions.Size();
  std::unique_ptr<TxTree[]> txTree(new TxTree[txNum]);

  // Build hashmap txid -> index
  std::unordered_map<BaseBlob<256>, size_t> txidMap;
  for (size_t i = 0; i < txNum; i++) {
    rapidjson::Value &txSrc = transactions[i];

    if (!txSrc.HasMember("data") || !txSrc["data"].IsString())
      return false;
    txTree[i].Data.HexData = txSrc["data"].GetString();
    txTree[i].Data.HexDataSize = txSrc["data"].GetStringLength();

    if (txSrc.HasMember("txid") && txSrc["txid"].IsString()) {
      txTree[i].Data.TxId.setHexLE(txSrc["txid"].GetString());
      if (txSrc.HasMember("hash"))
        txTree[i].Data.WitnessHash.setHexLE(txSrc["hash"].GetString());
    } else if (txSrc.HasMember("hash") && txSrc["hash"].IsString()) {
      txTree[i].Data.TxId.setHexLE(txSrc["hash"].GetString());
    } else {
      return false;
    }

    if (!txSrc.HasMember("fee") || !txSrc["fee"].IsInt64())
      return false;
    txTree[i].Fee = txSrc["fee"].GetUint64();

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
      return l.TxId.getHexLE() < r.TxId.getHexLE();
    });

  return true;
}

template<typename Proto, typename HeaderBuilderTy, typename CoinbaseBuilderTy, typename NotifyTy, typename PrepareForSubmitTy>
class WorkTy : public StratumSingleWork {
public:
  WorkTy(int64_t stratumWorkId, uint64_t uniqueWorkId, PoolBackend *backend, size_t backendIdx, const CMiningConfig &miningCfg, const std::vector<uint8_t> &miningAddress, const std::string &coinbaseMessage) :
    StratumSingleWork(stratumWorkId, uniqueWorkId, backend, backendIdx, miningCfg) {
    CoinbaseMessage_ = coinbaseMessage;
    this->Initialized_ = miningAddress.size() == sizeof(typename Proto::AddressTy);
    if (this->Initialized_)
      memcpy(MiningAddress_.begin(), &miningAddress[0], miningAddress.size());
  }
  virtual typename Proto::BlockHashTy shareHash() override { return Header.hash(); }
  virtual std::string blockHash(size_t) override { return Header.hash().getHexLE(); }
  virtual UInt<256> expectedWork(size_t) override { return Proto::expectedWork(Header, ConsensusCtx_); }
  virtual bool ready() override { return this->Backend_ != nullptr; }

  virtual void buildBlock(size_t, xmstream &blockHexData) override { buildBlockImpl(Header, CBTxWitness_, blockHexData); }

  virtual void mutate() override {
    Header.nTime = static_cast<uint32_t>(time(nullptr));
    buildNotifyMessageImpl(this, Header, JobVersion, CBTxLegacy_, MerklePath, this->MiningCfg_, true, this->NotifyMessage_);
  }

  virtual CCheckStatus checkConsensus(size_t, const UInt<256> &shareTarget) override { return checkConsensusImpl(Header, ConsensusCtx_, shareTarget); }

  virtual bool hasRtt(size_t) override { return ConsensusCtx_.hasRtt(); }

  virtual void buildNotifyMessage(bool resetPreviousWork) override {
    buildNotifyMessageImpl(this, Header, JobVersion, CBTxLegacy_, MerklePath, this->MiningCfg_, resetPreviousWork, this->NotifyMessage_);
  }

  virtual bool prepareForSubmit(const CWorkerConfig &workerCfg, const CStratumMessage &msg) override {
    return prepareForSubmitImpl(Header, JobVersion, CBTxLegacy_, CBTxWitness_, MerklePath, workerCfg, this->MiningCfg_, msg);
  }

  virtual bool loadFromTemplate(CBlockTemplate &blockTemplate, std::string &error) override {
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
    uint64_t blockRewardDelta = 0;
    bool txFilter = this->MiningCfg_.TxNumLimit && transactions.Size() > this->MiningCfg_.TxNumLimit;
    std::vector<TxData> processedTransactions;
    bool needSortByHash = (blockTemplate.Ticker == "BCHN" || blockTemplate.Ticker == "BCHABC");

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

    // Compute base block reward (subsidy without fees) for PPS
    {
      uint64_t totalFees = 0;
      for (rapidjson::SizeType i = 0, ie = transactions.Size(); i != ie; ++i) {
        if (transactions[i].HasMember("fee") && transactions[i]["fee"].IsUint64())
          totalFees += transactions[i]["fee"].GetUint64();
      }
      BaseBlockReward_ = this->BlockReward_ - totalFees;
    }

    this->BlockReward_ -= blockRewardDelta;

    // Calculate witness commitment
    if (SegwitEnabled) {
      if (!calculateWitnessCommitment(resultValue, txFilter, processedTransactions, WitnessCommitment, error))
        return false;
    }

    // Coinbase
    buildCoinbaseTx(nullptr, 0, this->MiningCfg_, CBTxLegacy_, CBTxWitness_);

    // Transactions
    collectTransactions(processedTransactions, TxHexData, MerklePath, this->TxNum_);

    // Mimble wimble data
    MimbleWimbleData.reset();
    if (resultValue.HasMember("mweb") && resultValue["mweb"].IsString())
      MimbleWimbleData.write(resultValue["mweb"].GetString(), resultValue["mweb"].GetStringLength());

    // Fill header
    if (!HeaderBuilderTy::build(Header, &JobVersion, CBTxLegacy_, MerklePath, resultValue)) {
      error = "missing header data";
      return false;
    }

    ConsensusCtx_.initialize(blockTemplate, blockTemplate.Ticker);
    return true;
  }

  virtual double getAbstractProfitValue(size_t, double price, double coeff) override {
    return price * this->BlockReward_ / Proto::getDifficulty(Header) * coeff;
  }

  UInt<384> blockReward(size_t) final { return fromRational(BlockReward_); }
  UInt<384> baseBlockReward(size_t) final { return fromRational(BaseBlockReward_); }

public:
  // Implementation
  /// Build & serialize custom coinbase transaction
  void buildCoinbaseTx(void *coinbaseData, size_t coinbaseSize, const CMiningConfig &miningCfg, CoinbaseTx &legacy, CoinbaseTx &witness) {
    CoinbaseBuilder_.build(this->Height_, this->BlockReward_, coinbaseData, coinbaseSize, this->CoinbaseMessage_, this->MiningAddress_, miningCfg, SegwitEnabled, WitnessCommitment, legacy, witness);
  }

  static CCheckStatus checkConsensusImpl(const typename Proto::BlockHeader &header, typename Proto::CheckConsensusCtx &consensusCtx, const UInt<256> &shareTarget) {
    typename Proto::ChainParams params;
    return Proto::checkConsensus(header, consensusCtx, params, shareTarget);
  }

  static void buildNotifyMessageImpl(StratumWork *source, typename Proto::BlockHeader &header, uint32_t asicBoostData, CoinbaseTx &legacy, const std::vector<BaseBlob<256>> &merklePath, const CMiningConfig &cfg, bool resetPreviousWork, xmstream &notifyMessage) {
    NotifyTy::build(source, header, asicBoostData, legacy, merklePath, cfg, resetPreviousWork, notifyMessage);
  }

  static bool prepareForSubmitImpl(typename Proto::BlockHeader &header, uint32_t asicBoostData, CoinbaseTx &legacy, CoinbaseTx &witness, const std::vector<BaseBlob<256>> &merklePath, const CWorkerConfig &workerCfg, const CMiningConfig &miningCfg, const CStratumMessage &msg) {
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

    // Mimble wimble
    if (MimbleWimbleData.sizeOf()) {
      blockHexData.write("01");
      blockHexData.write(MimbleWimbleData.data(), MimbleWimbleData.sizeOf());
    }
  }

public:
  // Header
  typename Proto::BlockHeader Header;
  // ASIC boost data
  uint32_t JobVersion;
  // Various block template data
  bool SegwitEnabled = false;
  std::vector<BaseBlob<256>> MerklePath;
  // Coinbase data
  typename Proto::AddressTy MiningAddress_;
  std::string CoinbaseMessage_;
  CoinbaseBuilderTy CoinbaseBuilder_;
  xmstream WitnessCommitment;
  CoinbaseTx CBTxLegacy_;
  CoinbaseTx CBTxWitness_;
  // Transaction data
  xmstream TxHexData;
  // mweb data
  xmstream MimbleWimbleData;
  // PoW check context
  typename Proto::CheckConsensusCtx ConsensusCtx_;

  uint64_t BlockReward_ = 0;
  uint64_t BaseBlockReward_ = 0;
};

}
