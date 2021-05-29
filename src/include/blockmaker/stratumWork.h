#pragma once

#include "rapidjson/document.h"
#include "poolcommon/jsonSerializer.h"
#include "poolinstances/stratumMsg.h"
#include <openssl/rand.h>

struct ThreadConfig {
  uint64_t ExtraNonceCurrent;
  unsigned ThreadsNum;
  void initialize(unsigned instanceId, unsigned instancesNum) {
    ExtraNonceCurrent = instanceId;
    ThreadsNum = instancesNum;
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
    if (!msg.stringId.empty())
      object.addString("id", msg.stringId);
    else
      object.addInt("id", msg.integerId);
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

  void onSubscribe(MiningConfig &miningCfg, StratumMessage &msg, xmstream &out, std::string &subscribeInfo) {
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

class StratumMergedWork;
class PoolBackend;
class StratumMessage;

class StratumWork {
public:
  StratumWork(uint64_t stratumId, const MiningConfig &miningCfg) : StratumId_(stratumId), MiningCfg_(miningCfg) {}
  virtual ~StratumWork() {}

  bool initialized() { return Initialized_; }
  virtual size_t backendsNum() = 0;
  virtual void shareHash(void *data) = 0;
  virtual std::string blockHash(size_t workIdx) = 0;
  virtual PoolBackend *backend(size_t workIdx) = 0;
  virtual size_t backendId(size_t workIdx) = 0;
  virtual uint64_t height(size_t workIdx) = 0;
  virtual size_t txNum(size_t workIdx) = 0;
  virtual int64_t blockReward(size_t workIdx) = 0;
  virtual double expectedWork(size_t workIdx) = 0;
  virtual void buildBlock(size_t workIdx, xmstream &stream) = 0;
  virtual bool ready() = 0;
  virtual void mutate() = 0;
  virtual bool checkConsensus(size_t workIdx, double *shareDiff) = 0;
  virtual void buildNotifyMessage(bool resetPreviousWork) = 0;
  virtual bool prepareForSubmit(const CWorkerConfig &workerCfg, const StratumMessage &msg) = 0;
  virtual double getAbstractProfitValue(size_t workIdx, double price, double coeff) = 0;
  // TODO: search better solution
  virtual bool resetNotRecommended() = 0;

  xmstream &notifyMessage() { return NotifyMessage_; }
  int64_t stratumId() const { return StratumId_; }
  void setStratumId(int64_t stratumId) { StratumId_ = stratumId; }

public:
  bool Initialized_ = false;
  int64_t StratumId_ = 0;
  unsigned SendCounter_ = 0;
  MiningConfig MiningCfg_;
  xmstream NotifyMessage_;
};

class StratumSingleWork : public StratumWork {
public:
  StratumSingleWork(int64_t stratumWorkId, uint64_t uniqueWorkId, PoolBackend *backend, size_t backendId, const MiningConfig &miningCfg) : StratumWork(stratumWorkId, miningCfg), UniqueWorkId_(uniqueWorkId), Backend_(backend), BackendId_(backendId) {}

  virtual size_t backendsNum() final { return 1; }
  virtual PoolBackend *backend(size_t) final { return Backend_; }
  virtual size_t backendId(size_t) final { return BackendId_; }
  virtual uint64_t height(size_t) final { return Height_; }
  virtual size_t txNum(size_t) final { return TxNum_; }
  virtual int64_t blockReward(size_t) final { return BlockReward_; }

  virtual bool loadFromTemplate(rapidjson::Value &document, const std::string &ticker, std::string &error) = 0;

  virtual ~StratumSingleWork();

  uint64_t uniqueWorkId() { return UniqueWorkId_; }

  void addLink(StratumMergedWork *mergedWork) {
    LinkedWorks_.push_back(mergedWork);
  }

  void clearLinks() {
    LinkedWorks_.clear();
  }

protected:
  uint64_t UniqueWorkId_ = 0;
  PoolBackend *Backend_ = nullptr;
  size_t BackendId_ = 0;
  std::vector<StratumMergedWork*> LinkedWorks_;
  uint64_t Height_ = 0;
  size_t TxNum_ = 0;
  int64_t BlockReward_ = 0;
};

class StratumMergedWork : public StratumWork {
public:
  StratumMergedWork(uint64_t stratumWorkId, StratumSingleWork *first, StratumSingleWork *second, const MiningConfig &miningCfg) : StratumWork(stratumWorkId, miningCfg) {
    Works_[0] = first;
    Works_[1] = second;
    first->addLink(this);
    second->addLink(this);
    Initialized_ = true;
  }

  virtual ~StratumMergedWork() {}

  virtual size_t backendsNum() final { return 2; }
  virtual PoolBackend *backend(size_t workIdx) final { return Works_[workIdx] ? Works_[workIdx]->backend(0) : nullptr; }
  virtual size_t backendId(size_t workIdx) final { return Works_[workIdx] ? Works_[workIdx]->backendId(0) : std::numeric_limits<size_t>::max(); }
  virtual uint64_t height(size_t workIdx) final { return Works_[workIdx]->height(0); }
  virtual size_t txNum(size_t workIdx) final { return Works_[workIdx]->txNum(0); }
  virtual int64_t blockReward(size_t workIdx) final { return Works_[workIdx]->blockReward(0); }
  virtual double expectedWork(size_t workIdx) final { return Works_[workIdx]->expectedWork(0); }
  virtual bool ready() final { return true; }
  virtual double getAbstractProfitValue(size_t workIdx, double price, double coeff) final { return Works_[workIdx]->getAbstractProfitValue(0, price, coeff); }
  virtual bool resetNotRecommended() final { return (Works_[0] ? Works_[0]->resetNotRecommended() : false) | (Works_[1] ? Works_[1]->resetNotRecommended() : false); }

  void removeLink(StratumSingleWork *work) {
    if (Works_[0] == work)
      Works_[0] = nullptr;
    if (Works_[1] == work)
      Works_[1] = nullptr;
  }

  bool empty() { return Works_[0] == nullptr && Works_[1] == nullptr; }

protected:
  StratumSingleWork *Works_[2] = {nullptr, nullptr};
};

class StratumSingleWorkEmpty : public StratumSingleWork {
public:
  StratumSingleWorkEmpty(int64_t stratumWorkId, uint64_t uniqueWorkId, PoolBackend *backend, size_t backendId, const MiningConfig &miningCfg, const std::vector<uint8_t>&, const std::string&) : StratumSingleWork(stratumWorkId, uniqueWorkId, backend, backendId, miningCfg) {}
  virtual void shareHash(void*) final {}
  virtual std::string blockHash(size_t) final { return std::string(); }
  virtual double expectedWork(size_t) final { return 0.0; }
  virtual void buildBlock(size_t, xmstream&) final {}
  virtual bool ready() final { return false; }
  virtual void mutate() final {}
  virtual bool checkConsensus(size_t, double*) final { return false; }
  virtual void buildNotifyMessage(bool) final {}
  virtual bool prepareForSubmit(const CWorkerConfig&, const StratumMessage&) final { return false; }
  virtual bool loadFromTemplate(rapidjson::Value&, const std::string&, std::string&) final { return false; }
  virtual double getAbstractProfitValue(size_t, double, double) final { return 0.0; }
  virtual bool resetNotRecommended() final { return false; }
};

class StratumMergedWorkEmpty : public StratumMergedWork {
public:
  StratumMergedWorkEmpty(uint64_t stratumWorkId, StratumSingleWork *first, StratumSingleWork *second, MiningConfig &cfg) : StratumMergedWork(stratumWorkId, first, second, cfg) {}
  virtual void shareHash(void*) final {}
  virtual std::string blockHash(size_t) final { return std::string(); }
  virtual void buildBlock(size_t, xmstream&) final {}
  virtual void mutate() final {}
  virtual void buildNotifyMessage(bool) final {}
  virtual bool prepareForSubmit(const CWorkerConfig&, const StratumMessage&) final { return false; }
  virtual bool checkConsensus(size_t, double*) final { return false; }
};
