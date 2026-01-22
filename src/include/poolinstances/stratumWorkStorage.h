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
    CStratumMessage Msg;

    bool HasShare = false;
  };

  using CWork = StratumWork;
  using CSingleWork = StratumSingleWork;
  using CMergedWork = StratumMergedWork;
  using CSingleWorkSequence = std::deque<std::unique_ptr<CSingleWork>>;
  using CMergedWorkSequence = std::deque<std::unique_ptr<CMergedWork>>;
  using CAcceptedShareSet = std::unordered_set<BaseBlob<256>>;
  using CWorkIndex = std::pair<size_t, size_t>;

private:
  static constexpr size_t WorkNotExists = std::numeric_limits<size_t>::max();

public:
  ~StratumWorkStorage() {
    for (size_t i = 0; i < BackendsNum_; i++) {
      for (const auto &work: PrimaryWorkStorage_[i])
        work->clearLinks();
    }
  }

  void init(const std::vector<PoolBackend*> &backends,
            const std::vector<size_t> &workResetList,
            const std::vector<size_t> &primaryBackends,
            const std::vector<size_t> &secondaryBackends) {
    BackendsNum_ = backends.size();
    Backends_ = backends;

    WorkResetMap_.resize(backends.size(), false);
    PrimaryBackendMap_.resize(backends.size(), false);
    SecondaryBackendMap_.resize(backends.size(), false);
    KnownWorkMap_.resize(backends.size(), false);

    PrimaryWorkStorage_.reset(new CSingleWorkSequence[BackendsNum_]);
    SecondaryWorkStorage_.reset(new CSingleWorkSequence[BackendsNum_]);
    MergedWorkStorage_.reset(new CMergedWorkSequence[BackendsNum_]);
    AcceptedShares_.reset(new CAcceptedShareSet[BackendsNum_]);
    PendingShares_.reset(new CPendingShare[BackendsNum_]);

    for (size_t i = 0, ie = backends.size(); i != ie; ++i)
      BackendMap_[backends[i]] = i;
    for (auto index: workResetList)
      WorkResetMap_[index] = true;
    for (auto index: primaryBackends)
      PrimaryBackendMap_[index] = true;
    for (auto index: secondaryBackends)
      SecondaryBackendMap_[index] = true;
  }

  size_t backendsNum() { return BackendsNum_; }
  bool isPrimaryBackend(size_t index) { return PrimaryBackendMap_[index]; }

  /// Returns work or nullptr if work not exists
  CWork *currentWork() { return CurrentWork_; }
  void setCurrentWork(CWork *work) { CurrentWork_ = work; }
  CPendingShare &pendingShare(size_t index) { return PendingShares_[index]; }

  CWork *workById(int64_t id) {
    return WorkIdMap_[id];
  }

  CWork *fetchWork(size_t index) {
    if (!MergedWorkStorage_[index].empty()) {
      CWork *work = MergedWorkStorage_[index].back().get();
      return work->ready() ? work : nullptr;
    }
    if (!PrimaryWorkStorage_[index].empty()) {
      CWork *work = PrimaryWorkStorage_[index].back().get();
      return work->ready() ? work : nullptr;
    }

    return nullptr;
  }

  void createWork(CBlockTemplate &blockTemplate,
                  PoolBackend *backend,
                  const std::string &ticker,
                  const std::vector<uint8_t> &miningAddress,
                  const std::string &coinbaseMsg,
                  CMiningConfig &miningConfig,
                  const std::string &stratumInstanceName) {
    auto It = BackendMap_.find(backend);
    if (It == BackendMap_.end())
      return;
    size_t backendIdx = It->second;


    if (PrimaryBackendMap_[backendIdx]) {
      std::string error;
      CSingleWork *primaryWork = X::Stratum::newPrimaryWork(newStratumId(),
                                                           backend,
                                                           backendIdx,
                                                           miningConfig,
                                                           miningAddress,
                                                           coinbaseMsg,
                                                           blockTemplate,
                                                           error);
      if (primaryWork) {
        pushSingleWork(primaryWork, PrimaryWorkStorage_[backendIdx], backendIdx);

        if (X::Stratum::MergedMiningSupport) {
          // create merged mining work with current coin
          // collect all secondary coins
          std::vector<CSingleWork*> secondaryWorks;
          for (size_t i = 0; i < BackendsNum_; i++) {
            CSingleWorkSequence &secondarySequence = SecondaryWorkStorage_[i];
            if (SecondaryBackendMap_[i] && i != backendIdx && !secondarySequence.empty())
              secondaryWorks.push_back(secondarySequence.back().get());
          }

          if (!secondaryWorks.empty()) {
            // create merged work using (primaryWork, secondaryWorks)
            CMergedWork *mergedWork = X::Stratum::newMergedWork(newStratumId(),
                                                                primaryWork,
                                                                secondaryWorks,
                                                                miningConfig,
                                                                error);
            if (mergedWork) {
              pushMergedWork(mergedWork, backendIdx);
            } else if (!error.empty()) {
              std::string tickers;
              LOG_F(ERROR, "%s: can't create merged work error: %s", stratumInstanceName.c_str(), error.c_str());
            }
          }
        }
      } else if (!error.empty()) {
        LOG_F(ERROR, "%s: can't create primary work for %s; error: %s", stratumInstanceName.c_str(), ticker.c_str(), error.c_str());
      }
    }

    if (SecondaryBackendMap_[backendIdx]) {
      std::string error;
      CSingleWork *secondaryWork = X::Stratum::newSecondaryWork(newStratumId(),
                                                             backend,
                                                             backendIdx,
                                                             miningConfig,
                                                             miningAddress,
                                                             coinbaseMsg,
                                                             blockTemplate,
                                                             error);
      if (secondaryWork) {
        pushSingleWork(secondaryWork, SecondaryWorkStorage_[backendIdx], backendIdx);

        // update all merged works
        for (size_t primaryIdx = 0; primaryIdx < BackendsNum_; primaryIdx++) {
          if (!PrimaryBackendMap_[primaryIdx])
            continue;
          CSingleWorkSequence &primarySequence = PrimaryWorkStorage_[primaryIdx];
          if (primarySequence.empty())
            continue;

          CSingleWork *primaryWork = primarySequence.back().get();
          std::vector<CSingleWork*> secondaryWorks;
          for (size_t secondaryIdx = 0; secondaryIdx < BackendsNum_; secondaryIdx++) {
            if (secondaryIdx == primaryIdx || !SecondaryBackendMap_[secondaryIdx])
              continue;
            CSingleWorkSequence &secondarySequence = SecondaryWorkStorage_[secondaryIdx];
            if (secondarySequence.empty())
              continue;
            secondaryWorks.push_back(secondarySequence.back().get());
          }

          if (secondaryWorks.empty())
            continue;

          // create merged work using (primaryWork, secondaryWorks)
          CMergedWork *mergedWork = X::Stratum::newMergedWork(newStratumId(),
                                                              primaryWork,
                                                              secondaryWorks,
                                                              miningConfig,
                                                              error);
          if (mergedWork) {
            pushMergedWork(mergedWork, primaryIdx);
          } else if (!error.empty()) {
            std::string tickers;
            LOG_F(ERROR, "%s: can't create merged work error: %s", stratumInstanceName.c_str(), error.c_str());
          }
        }
      } else if (!error.empty()) {
        LOG_F(ERROR, "%s: can't create secondary work for %s; error: %s", stratumInstanceName.c_str(), ticker.c_str(), error.c_str());
      }
    }
  }

  bool needSendResetSignal(CWork *work) {
    bool result = false;
    for (unsigned i = 0; i < work->backendsNum(); i++) {
      unsigned backendId = work->backendId(i);
      if (KnownWorkMap_[backendId] == false && WorkResetMap_[backendId] == true)
        result = true;
      KnownWorkMap_[backendId] = true;
    }

    return result;
  }

  bool isDuplicate(CWork *work, const BaseBlob<256> &shareHash) {
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
                     const CStratumMessage &msg) {
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
  std::vector<PoolBackend*> Backends_;
  std::vector<bool> WorkResetMap_;
  std::vector<bool> PrimaryBackendMap_;
  std::vector<bool> SecondaryBackendMap_;
  std::vector<bool> KnownWorkMap_;
  std::unordered_map<PoolBackend*, size_t> BackendMap_;

  std::unique_ptr<CMergedWorkSequence[]> MergedWorkStorage_;
  std::unique_ptr<CSingleWorkSequence[]> PrimaryWorkStorage_;
  std::unique_ptr<CSingleWorkSequence[]> SecondaryWorkStorage_;
  std::unique_ptr<CAcceptedShareSet[]> AcceptedShares_;
  std::unique_ptr<CPendingShare[]> PendingShares_;
  std::unordered_map<int64_t, CWork*> WorkIdMap_;

  int64_t LastStratumId_ = 0;

private:
  int64_t newStratumId() {
    LastStratumId_ = std::max(LastStratumId_ + 1, static_cast<int64_t>(time(nullptr)));
    return LastStratumId_;
  }

  void pushSingleWork(CSingleWork *work, CSingleWorkSequence &sequence, size_t backendIdx) {
    if (!sequence.empty() && sequence.back()->uniqueWorkId() != work->uniqueWorkId()) {
      KnownWorkMap_[backendIdx] = false;
      eraseAll(sequence, backendIdx);
    }

    WorkIdMap_[work->stratumId()] = work;
    sequence.emplace_back(work);

    // Remove old work
    while (sequence.size() > WorksetSizeLimit)
      eraseFirst(sequence);
  }

  void pushMergedWork(CMergedWork *work, size_t backendIdx) {
    CMergedWorkSequence &sequence = MergedWorkStorage_[backendIdx];
    WorkIdMap_[work->stratumId()] = work;
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
