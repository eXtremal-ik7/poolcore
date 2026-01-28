#pragma once

#include "poolcommon/baseBlob.h"
#include "poolcommon/uint.h"
#include "poolcore/blockTemplate.h"
#include "poolinstances/stratumMsg.h"
#include "p2putils/xmstream.h"
#include <string>
#include <vector>

struct ThreadConfig {
  uint64_t ExtraNonceCurrent;
  unsigned ThreadsNum;
  void initialize(unsigned instanceId, unsigned instancesNum) {
    ExtraNonceCurrent = instanceId;
    ThreadsNum = instancesNum;
  }
};

struct CMiningConfig {
  unsigned FixedExtraNonceSize;
  unsigned MutableExtraNonceSize;
  unsigned TxNumLimit;
};

// Worker config contains set of fields for all supported stratums
struct CWorkerConfig {
  std::string SetDifficultySession;
  std::string NotifySession;
  uint64_t ExtraNonceFixed;
  // SHA-256 only ASIC-boost fields
  bool AsicBoostEnabled = false;
  uint32_t VersionMask = 0;
};

struct CCheckStatus {
  bool IsBlock = false;
  bool IsShare = false;
  // For XEC rtt
  bool IsPendingBlock = false;
  UInt<256> Hash;
};

class StratumMergedWork;
class PoolBackend;

class StratumWork {
public:
  StratumWork(uint64_t stratumId, const CMiningConfig &miningCfg) : StratumId_(stratumId), MiningCfg_(miningCfg) {}
  virtual ~StratumWork() {}

  bool initialized() { return Initialized_; }
  virtual size_t backendsNum() = 0;
  virtual BaseBlob<256> shareHash() = 0;
  virtual std::string blockHash(size_t workIdx) = 0;
  virtual PoolBackend *backend(size_t workIdx) = 0;
  virtual size_t backendId(size_t workIdx) = 0;
  virtual uint64_t height(size_t workIdx) = 0;
  virtual size_t txNum(size_t workIdx) = 0;
  virtual UInt<384> blockReward(size_t workIdx) = 0;
  virtual UInt<256> expectedWork(size_t workIdx) = 0;
  virtual void buildBlock(size_t workIdx, xmstream &stream) = 0;
  virtual bool ready() = 0;
  virtual void mutate() = 0;
  virtual CCheckStatus checkConsensus(size_t workIdx, const UInt<256> &shareTarget) = 0;
  virtual void buildNotifyMessage(bool resetPreviousWork) = 0;
  virtual bool prepareForSubmit(const CWorkerConfig &workerCfg, const CStratumMessage &msg) = 0;
  virtual double getAbstractProfitValue(size_t workIdx, double price, double coeff) = 0;
  virtual bool hasRtt(size_t workIdx) = 0;

  xmstream &notifyMessage() { return NotifyMessage_; }
  int64_t stratumId() const { return StratumId_; }
  void setStratumId(int64_t stratumId) { StratumId_ = stratumId; }

public:
  bool Initialized_ = false;
  int64_t StratumId_ = 0;
  unsigned SendCounter_ = 0;
  CMiningConfig MiningCfg_;
  xmstream NotifyMessage_;
};

class StratumSingleWork : public StratumWork {
public:
  StratumSingleWork(int64_t stratumWorkId, uint64_t uniqueWorkId, PoolBackend *backend, size_t backendId, const CMiningConfig &miningCfg) :
      StratumWork(stratumWorkId, miningCfg), UniqueWorkId_(uniqueWorkId), Backend_(backend), BackendId_(backendId) {}

  virtual size_t backendsNum() final { return 1; }
  virtual PoolBackend *backend(size_t) final { return Backend_; }
  virtual size_t backendId(size_t) final { return BackendId_; }
  virtual uint64_t height(size_t) final { return Height_; }
  virtual size_t txNum(size_t) final { return TxNum_; }
  // virtual UInt<384> blockReward(size_t) final { return BlockReward_; }

  virtual bool loadFromTemplate(CBlockTemplate &blockTemplate, std::string &error) = 0;

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
};

class StratumMergedWork : public StratumWork {
public:
  struct CWorkWithId {
    StratumSingleWork *Work;
    size_t Id;
  };

public:
  StratumMergedWork(uint64_t stratumWorkId,
                    StratumSingleWork *first,
                    StratumSingleWork *second,
                    const CMiningConfig &miningCfg) : StratumWork(stratumWorkId, miningCfg) {
    Works_.resize(2);
    Works_[0].Work = first;
    Works_[0].Id = first->backendId(0);
    Works_[1].Work = second;
    Works_[1].Id = second->backendId(0);
    first->addLink(this);
    second->addLink(this);
    this->Initialized_ = true;
  }

  StratumMergedWork(uint64_t stratumWorkId,
                    StratumSingleWork *primary,
                    std::vector<StratumSingleWork*> &secondary,
                    const CMiningConfig &miningCfg) : StratumWork(stratumWorkId, miningCfg) {
    Works_.resize(1 + secondary.size());
    Works_[0].Work = primary;
    Works_[0].Id = primary->backendId(0);
    primary->addLink(this);
    for (size_t i = 0; i < secondary.size(); i++) {
      Works_[i + 1].Work = secondary[i];
      Works_[i + 1].Id = secondary[i]->backendId(0);
      secondary[i]->addLink(this);
    }

    this->Initialized_ = true;
  }

  virtual ~StratumMergedWork() {}

  virtual size_t backendsNum() final { return Works_.size(); }
  virtual PoolBackend *backend(size_t workIdx) final { return Works_[workIdx].Work ? Works_[workIdx].Work->backend(0) : nullptr; }
  virtual size_t backendId(size_t workIdx) final { return Works_[workIdx].Id; }
  virtual uint64_t height(size_t workIdx) final { return Works_[workIdx].Work->height(0); }
  virtual size_t txNum(size_t workIdx) final { return Works_[workIdx].Work->txNum(0); }
  virtual UInt<384> blockReward(size_t workIdx) final { return Works_[workIdx].Work->blockReward(0); }
  virtual UInt<256> expectedWork(size_t workIdx) final { return Works_[workIdx].Work->expectedWork(0); }
  virtual bool ready() final { return true; }
  virtual double getAbstractProfitValue(size_t workIdx, double price, double coeff) final { return Works_[workIdx].Work->getAbstractProfitValue(0, price, coeff); }
  virtual bool hasRtt(size_t workIdx) final { return Works_[workIdx].Work->hasRtt(0); }

  void removeLink(StratumSingleWork *work) {
    for (auto &w: Works_) {
      if (w.Work == work) {
        w.Work = nullptr;
        break;
      }
    }
  }

  bool empty() {
    for (auto &w: Works_) {
      if (w.Work)
        return false;
    }

    return true;
  }

protected:
  std::vector<CWorkWithId> Works_;
};

class StratumSingleWorkEmpty : public StratumSingleWork {
public:
  StratumSingleWorkEmpty(int64_t stratumWorkId,
                         uint64_t uniqueWorkId,
                         PoolBackend *backend,
                         size_t backendId,
                         const CMiningConfig &miningCfg,
                         const std::vector<uint8_t>&,
                         const std::string&) : StratumSingleWork(stratumWorkId, uniqueWorkId, backend, backendId, miningCfg) {}
  virtual BaseBlob<256> shareHash() final { return BaseBlob<256>::zero(); }
  virtual std::string blockHash(size_t) final { return std::string(); }
  virtual UInt<256> expectedWork(size_t) final { return UInt<256>::zero(); }
  virtual void buildBlock(size_t, xmstream&) final {}
  virtual bool ready() final { return false; }
  virtual void mutate() final {}
  virtual CCheckStatus checkConsensus(size_t, const UInt<256>&) final { return CCheckStatus(); }
  virtual void buildNotifyMessage(bool) final {}
  virtual bool prepareForSubmit(const CWorkerConfig&, const CStratumMessage&) final { return false; }
  virtual bool loadFromTemplate(CBlockTemplate&, std::string&) final { return false; }
  virtual double getAbstractProfitValue(size_t, double, double) final { return 0.0; }
  virtual bool resetNotRecommended() final { return false; }
  virtual bool hasRtt(size_t) final { return false; }
};

class StratumMergedWorkEmpty : public StratumMergedWork {
public:
  StratumMergedWorkEmpty(uint64_t stratumWorkId,
                         StratumSingleWork *first,
                         StratumSingleWork *second,
                         CMiningConfig &cfg) : StratumMergedWork(stratumWorkId, first, second, cfg) {}
  virtual BaseBlob<256> shareHash() final { return BaseBlob<256>::zero(); }
  virtual std::string blockHash(size_t) final { return std::string(); }
  virtual void buildBlock(size_t, xmstream&) final {}
  virtual void mutate() final {}
  virtual void buildNotifyMessage(bool) final {}
  virtual bool prepareForSubmit(const CWorkerConfig&, const CStratumMessage&) final { return false; }
  virtual CCheckStatus checkConsensus(size_t, const UInt<256>&) final { return CCheckStatus(); }
};
