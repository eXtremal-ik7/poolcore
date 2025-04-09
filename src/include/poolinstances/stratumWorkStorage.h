#pragma once

#include "blockmaker/stratumWork.h"
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "loguru.hpp"

template<typename X>
class StratumWorkStorage {
public:
  struct CPendingShare {
    std::string User;
    std::string WorkerName;
    double RealDifficulty = 0.0;
    double StratumDifficulty;
    int64_t MajorJobId;
    CWorkerConfig WorkerConfig;
    typename X::Stratum::StratumMessage Msg;

    bool HasShare = false;
  };

  using CWork = StratumWork<typename X::Proto::BlockHashTy, typename X::Stratum::MiningConfig, typename X::Stratum::StratumMessage>;
  using CSingleWork = StratumSingleWork<typename X::Proto::BlockHashTy, typename X::Stratum::MiningConfig, typename X::Stratum::StratumMessage>;
  using CMergedWork = StratumMergedWork<typename X::Proto::BlockHashTy, typename X::Stratum::MiningConfig, typename X::Stratum::StratumMessage>;
  using CSingleWorkSequence = std::deque<std::unique_ptr<CSingleWork>>;
  using CMergedWorkSequence = std::deque<std::unique_ptr<CMergedWork>>;
  using CAcceptedShareSet = std::unordered_set<typename X::Proto::BlockHashTy>;
  using CWorkIndex = std::pair<size_t, size_t>;

private:
  static constexpr size_t WorkNotExists = std::numeric_limits<size_t>::max();

public:
  ~StratumWorkStorage() {
    for (size_t i = 0; i < BackendsNum_; i++) {
      for (const auto &work: WorkStorage_[i])
        work->clearLinks();
    }
  }

  void init(const std::vector<std::pair<PoolBackend*, bool>> &backends) {
    BackendsNum_ = backends.size();
    WorkStorage_.reset(new CSingleWorkSequence[BackendsNum_]);
    MergedWorkStorage_.reset(new CMergedWorkSequence[BackendsNum_ * BackendsNum_]);
    AcceptedShares_.reset(new CAcceptedShareSet[BackendsNum_]);
    PendingShares_.reset(new CPendingShare[BackendsNum_]);
    FirstBackends_.reset(new bool[BackendsNum_]);

    for (size_t i = 0, ie = backends.size(); i != ie; ++i) {
      BackendMap_[backends[i].first] = i;
      FirstBackends_[i] = backends[i].second;
    }
  }

  size_t backendsNum() { return BackendsNum_; }

  /// Returns work or nullptr if work not exists
  CWork *currentWork() { return CurrentWork_; }
  CWork *lastAcceptedWork() { return LastAcceptedWork_; }
  void setCurrentWork(CWork *work) { CurrentWork_ = work; }
  CPendingShare &pendingShare(size_t index) { return PendingShares_[index]; }

  CWork *workById(int64_t id) {
    return WorkIdMap_[id];
  }

  CSingleWork *singleWork(size_t index) {
    CSingleWorkSequence &sequence = WorkStorage_[index];
    return !sequence.empty() ? sequence.back().get() : nullptr;
  }

  CMergedWork *mergedWork(size_t i, size_t j) {
    CMergedWorkSequence &sequence = MergedWorkStorage_[i * BackendsNum_ + j];
    return !sequence.empty() ? sequence.back().get() : nullptr;
  }

  /// Add work
  bool createWork(CBlockTemplate &blockTemplate, PoolBackend *backend, const std::string &ticker, const std::vector<uint8_t> &miningAddress, const std::string &coinbaseMsg, typename X::Stratum::MiningConfig &miningConfig, const std::string &stratumInstanceName, bool *isNewBlock) {
    auto It = BackendMap_.find(backend);
    if (It == BackendMap_.end())
      return false;
    size_t backendIdx = It->second;

    std::string error;
    CSingleWork *work = newSingleWork(backend, backendIdx, blockTemplate.UniqueWorkId, miningConfig, miningAddress, coinbaseMsg);
    if (!work->initialized()) {
      LOG_F(ERROR, "%s: work create impossible for %s", stratumInstanceName.c_str(), ticker.c_str());
      return false;
    }
    if (!work->loadFromTemplate(blockTemplate, ticker, error)) {
      LOG_F(ERROR, "%s: can't process block template; error: %s", stratumInstanceName.c_str(), error.c_str());
      return false;
    }

    // Create merged work if need
    if (X::Stratum::MergedMiningSupport) {
      bool isFirstBackend = FirstBackends_[backendIdx];
      for (unsigned i = 0; i < BackendsNum_; i++) {
        // Can't create merged work using 2 backends with same type
        if ((isFirstBackend ^ FirstBackends_[i]) == 0)
          continue;

        size_t firstIdx = isFirstBackend ? backendIdx : i;
        size_t secondIdx = isFirstBackend ? i : backendIdx;

        CSingleWorkSequence &secondSequence = WorkStorage_[i];
        if (secondSequence.empty())
          continue;

        CSingleWork *mainWork = isFirstBackend ? work : secondSequence.back().get();
        CSingleWork *extraWork = isFirstBackend ? secondSequence.back().get() : work;
        newMergedWork(firstIdx, secondIdx, mainWork, extraWork, miningConfig);
      }
    }

    if (CurrentWork_) {
      bool currentWorkAffected = CurrentWork_->backendId(0) == backendIdx || CurrentWork_->backendId(1) == backendIdx;
      bool blockUpdated = WorkStorage_[backendIdx].size() == 1;
      *isNewBlock = currentWorkAffected & blockUpdated;
    } else {
      *isNewBlock = true;
    }

    return true;
  }

  bool isDuplicate(CWork *work, const typename X::Proto::BlockHashTy &shareHash) {
    bool duplicate = false;
    for (size_t i = 0, ie = work->backendsNum(); i != ie; ++i)
      duplicate |= !AcceptedShares_[work->backendId(i)].insert(shareHash).second;
    return duplicate;
  }

  bool updatePending(size_t index,
                     const std::string &user,
                     const std::string &workerName,
                     double realDifficulty,
                     double stratumDifficulty,
                     int64_t majorJobId,
                     const CWorkerConfig &workerConfig,
                     const typename X::Stratum::StratumMessage &msg) {
    CPendingShare &share = PendingShares_[index];
    if (!share.HasShare || share.RealDifficulty < realDifficulty) {
      share.User = user;
      share.WorkerName = workerName;
      share.RealDifficulty = realDifficulty;
      share.StratumDifficulty = stratumDifficulty;
      share.MajorJobId = majorJobId;
      share.WorkerConfig = workerConfig;
      share.Msg = msg;
      share.HasShare = true;
      return true;
    } else {
      return false;
    }
  }

private:
  size_t BackendsNum_ = 0;
  CWork *CurrentWork_= nullptr;
  CWork *LastAcceptedWork_ = nullptr;
  std::unordered_map<PoolBackend*, size_t> BackendMap_;
  std::unique_ptr<bool[]> FirstBackends_;

  std::unique_ptr<CSingleWorkSequence[]> WorkStorage_;
  std::unique_ptr<CMergedWorkSequence[]> MergedWorkStorage_;
  std::unique_ptr<CAcceptedShareSet[]> AcceptedShares_;
  std::unique_ptr<CPendingShare[]> PendingShares_;
  std::unordered_map<int64_t, CWork*> WorkIdMap_;

  int64_t lastStratumId = 0;

private: 
  CSingleWork *newSingleWork(PoolBackend *backend, size_t backendIdx, uint64_t uniqueId, const typename X::Stratum::MiningConfig &miningCfg, const std::vector<uint8_t> &miningAddress, const std::string &coinbaseMessage) {
    lastStratumId = std::max(lastStratumId+1, static_cast<int64_t>(time(nullptr)));
    CSingleWork *work;
    if (FirstBackends_[backendIdx])
      work = new typename X::Stratum::Work(lastStratumId, uniqueId, backend, backendIdx, miningCfg, miningAddress, coinbaseMessage);
    else
      work = new typename X::Stratum::SecondWork(lastStratumId, uniqueId, backend, backendIdx, miningCfg, miningAddress, coinbaseMessage);

    CSingleWorkSequence &sequence = WorkStorage_[backendIdx];
    if (!sequence.empty() && sequence.back()->uniqueWorkId() != uniqueId)
      eraseAll(sequence, backendIdx);

    WorkIdMap_[lastStratumId] = work;
    LastAcceptedWork_ = work;
    sequence.emplace_back(work);

    // Remove old work
    while (sequence.size() > WorksetSizeLimit)
      eraseFirst(sequence);

    return work;
  }

  void newMergedWork(size_t firstIdx, size_t secondIdx, CSingleWork *first, CSingleWork *second, typename X::Stratum::MiningConfig &miningCfg) {
    lastStratumId = std::max(lastStratumId+1, static_cast<int64_t>(time(nullptr)));
    CMergedWork *work = new typename X::Stratum::MergedWork(lastStratumId, first, second, miningCfg);
    WorkIdMap_[lastStratumId] = work;
    LastAcceptedWork_ = work;

    CMergedWorkSequence &sequence = MergedWorkStorage_[firstIdx * BackendsNum_ + secondIdx];
    sequence.emplace_back(work);

    // Cleanup
    while (!sequence.empty()) {
      if (sequence.front()->empty())
        eraseFirst(sequence);
      else
        break;
    }
  }

  template<typename T> void eraseFirst(std::deque<T> &sequence) {
    if (CurrentWork_ == sequence.front().get())
      CurrentWork_ = nullptr;
    WorkIdMap_.erase(sequence.front()->stratumId());
    sequence.pop_front();
  }

  template<typename T> void eraseAll(std::deque<T> &sequence, size_t backendIdx) {
    for (const auto &work: sequence) {
      WorkIdMap_.erase(work->stratumId());
      if (CurrentWork_ == work.get())
        CurrentWork_ = nullptr;
    }

    sequence.clear();
    AcceptedShares_[backendIdx].clear();
    PendingShares_[backendIdx].HasShare = false;
    PendingShares_[backendIdx].RealDifficulty = 0.0;
  }

private:
  static constexpr size_t WorksetSizeLimit = 3;
};



