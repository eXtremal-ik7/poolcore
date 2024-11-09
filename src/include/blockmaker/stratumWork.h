#pragma once

#include "poolcore/blockTemplate.h"
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

struct CCheckStatus {
  bool IsBlock = false;
  double ShareDiff = 0.0;
  // For XEC rtt
  bool IsPendingBlock = false;

};

template<typename BlockHashTy, typename MiningConfig, typename WorkerConfig, typename StratumMessage>
class StratumMergedWork;
class PoolBackend;

template<typename BlockHashTy, typename MiningConfig, typename WorkerConfig, typename StratumMessage>
class StratumWork {
public:
  StratumWork(uint64_t stratumId, const MiningConfig &miningCfg) : StratumId_(stratumId), MiningCfg_(miningCfg) {}
  virtual ~StratumWork() {}

  bool initialized() { return Initialized_; }
  virtual size_t backendsNum() = 0;
  virtual BlockHashTy shareHash() = 0;
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
  virtual CCheckStatus checkConsensus(size_t workIdx) = 0;
  virtual void buildNotifyMessage(bool resetPreviousWork) = 0;
  virtual bool prepareForSubmit(const WorkerConfig &workerCfg, const StratumMessage &msg) = 0;
  virtual double getAbstractProfitValue(size_t workIdx, double price, double coeff) = 0;
  virtual bool hasRtt(size_t workIdx) = 0;

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

template<typename BlockHashTy, typename MiningConfig, typename WorkerConfig, typename StratumMessage>
class StratumSingleWork : public StratumWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage> {
public:
  StratumSingleWork(int64_t stratumWorkId, uint64_t uniqueWorkId, PoolBackend *backend, size_t backendId, const MiningConfig &miningCfg) : StratumWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>(stratumWorkId, miningCfg), UniqueWorkId_(uniqueWorkId), Backend_(backend), BackendId_(backendId) {}

  virtual size_t backendsNum() final { return 1; }
  virtual PoolBackend *backend(size_t) final { return Backend_; }
  virtual size_t backendId(size_t) final { return BackendId_; }
  virtual uint64_t height(size_t) final { return Height_; }
  virtual size_t txNum(size_t) final { return TxNum_; }
  virtual int64_t blockReward(size_t) final { return BlockReward_; }

  virtual bool loadFromTemplate(CBlockTemplate &blockTemplate, const std::string &ticker, std::string &error) = 0;

  virtual ~StratumSingleWork() {
    for (auto work: LinkedWorks_)
      work->removeLink(this);
  }

  uint64_t uniqueWorkId() { return UniqueWorkId_; }

  void addLink(StratumMergedWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage> *mergedWork) {
    LinkedWorks_.push_back(mergedWork);
  }

  void clearLinks() {
    LinkedWorks_.clear();
  }

protected:
  uint64_t UniqueWorkId_ = 0;
  PoolBackend *Backend_ = nullptr;
  size_t BackendId_ = 0;
  std::vector<StratumMergedWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>*> LinkedWorks_;
  uint64_t Height_ = 0;
  size_t TxNum_ = 0;
  int64_t BlockReward_ = 0;
};

template<typename BlockHashTy, typename MiningConfig, typename WorkerConfig, typename StratumMessage>
class StratumMergedWork : public StratumWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage> {
public:
  StratumMergedWork(uint64_t stratumWorkId,
                    StratumSingleWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage> *first,
                    StratumSingleWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage> *second,
                    const MiningConfig &miningCfg) : StratumWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>(stratumWorkId, miningCfg) {
    Works_[0] = first;
    Works_[1] = second;
    WorkId_[0] = first->backendId(0);
    WorkId_[1] = second->backendId(0);
    first->addLink(this);
    second->addLink(this);
    this->Initialized_ = true;
  }

  virtual ~StratumMergedWork() {}

  virtual size_t backendsNum() final { return 2; }
  virtual PoolBackend *backend(size_t workIdx) final { return Works_[workIdx] ? Works_[workIdx]->backend(0) : nullptr; }
  virtual size_t backendId(size_t workIdx) final { return WorkId_[workIdx]; }
  virtual uint64_t height(size_t workIdx) final { return Works_[workIdx]->height(0); }
  virtual size_t txNum(size_t workIdx) final { return Works_[workIdx]->txNum(0); }
  virtual int64_t blockReward(size_t workIdx) final { return Works_[workIdx]->blockReward(0); }
  virtual double expectedWork(size_t workIdx) final { return Works_[workIdx]->expectedWork(0); }
  virtual bool ready() final { return true; }
  virtual double getAbstractProfitValue(size_t workIdx, double price, double coeff) final { return Works_[workIdx]->getAbstractProfitValue(0, price, coeff); }
  virtual bool hasRtt(size_t workIdx) final { return Works_[workIdx]->hasRtt(0); }

  void removeLink(StratumSingleWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage> *work) {
    if (Works_[0] == work)
      Works_[0] = nullptr;
    if (Works_[1] == work)
      Works_[1] = nullptr;
  }

  bool empty() { return Works_[0] == nullptr && Works_[1] == nullptr; }

protected:
  StratumSingleWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage> *Works_[2] = {nullptr, nullptr};
  size_t WorkId_[2] = {std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max()};
};

template<typename BlockHashTy, typename MiningConfig, typename WorkerConfig, typename StratumMessage>
class StratumSingleWorkEmpty : public StratumSingleWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage> {
public:
  StratumSingleWorkEmpty(int64_t stratumWorkId,
                         uint64_t uniqueWorkId,
                         PoolBackend *backend,
                         size_t backendId,
                         const MiningConfig &miningCfg,
                         const std::vector<uint8_t>&,
                         const std::string&) : StratumSingleWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>(stratumWorkId, uniqueWorkId, backend, backendId, miningCfg) {}
  virtual BlockHashTy shareHash() final { return BlockHashTy(); }
  virtual std::string blockHash(size_t) final { return std::string(); }
  virtual double expectedWork(size_t) final { return 0.0; }
  virtual void buildBlock(size_t, xmstream&) final {}
  virtual bool ready() final { return false; }
  virtual void mutate() final {}
  virtual CCheckStatus checkConsensus(size_t) final { return CCheckStatus(); }
  virtual void buildNotifyMessage(bool) final {}
  virtual bool prepareForSubmit(const WorkerConfig&, const StratumMessage&) final { return false; }
  virtual bool loadFromTemplate(CBlockTemplate&, const std::string&, std::string&) final { return false; }
  virtual double getAbstractProfitValue(size_t, double, double) final { return 0.0; }
  virtual bool resetNotRecommended() final { return false; }
  virtual bool hasRtt(size_t) final { return false; }
};

template<typename BlockHashTy, typename MiningConfig, typename WorkerConfig, typename StratumMessage>
class StratumMergedWorkEmpty : public StratumMergedWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage> {
public:
  StratumMergedWorkEmpty(uint64_t stratumWorkId,
                         StratumSingleWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage> *first,
                         StratumSingleWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage> *second,
                         MiningConfig &cfg) : StratumMergedWork<BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>(stratumWorkId, first, second, cfg) {}
  virtual BlockHashTy shareHash() final { return BlockHashTy(); }
  virtual std::string blockHash(size_t) final { return std::string(); }
  virtual void buildBlock(size_t, xmstream&) final {}
  virtual void mutate() final {}
  virtual void buildNotifyMessage(bool) final {}
  virtual bool prepareForSubmit(const WorkerConfig&, const StratumMessage&) final { return false; }
  virtual CCheckStatus checkConsensus(size_t) final { return CCheckStatus(); }
};
