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
  using CSingleWorkSequence = std::deque<std::unique_ptr<StratumSingleWork>>;
  using CMergedWorkSequence = std::deque<std::unique_ptr<StratumMergedWork>>;
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
    FirstBackends_.reset(new bool[BackendsNum_]);

    for (size_t i = 0, ie = backends.size(); i != ie; ++i) {
      BackendMap_[backends[i].first] = i;
      FirstBackends_[i] = backends[i].second;
    }
  }

  /// Returns work or nullptr if work not exists
  StratumWork *currentWork() { return CurrentWork_; }
  StratumWork *lastAcceptedWork() { return LastAcceptedWork_; }
  void setCurrentWork(StratumWork *work) { CurrentWork_ = work; }

  StratumWork *workById(int64_t id) {
    return WorkIdMap_[id];
  }

  StratumSingleWork *singleWork(size_t index) {
    CSingleWorkSequence &sequence = WorkStorage_[index];
    return !sequence.empty() ? sequence.back().get() : nullptr;
  }

  StratumMergedWork *mergedWork(size_t i, size_t j) {
    CMergedWorkSequence &sequence = MergedWorkStorage_[i * BackendsNum_ + j];
    return !sequence.empty() ? sequence.back().get() : nullptr;
  }

  /// Add work
  bool createWork(rapidjson::Document &document, uint64_t uniqueWorkId, PoolBackend *backend, const std::string &ticker, const std::vector<uint8_t> &miningAddress, const std::string &coinbaseMsg, MiningConfig &miningConfig, const std::string &stratumInstanceName, bool *newBlock) {
    auto It = BackendMap_.find(backend);
    if (It == BackendMap_.end())
      return false;
    size_t backendIdx = It->second;

    std::string error;
    StratumSingleWork *work = newSingleWork(backend, backendIdx, uniqueWorkId, miningConfig, miningAddress, coinbaseMsg, newBlock);
    if (!work->initialized()) {
      LOG_F(ERROR, "%s: work create impossible for %s", stratumInstanceName.c_str(), ticker.c_str());
      return false;
    }
    if (!work->loadFromTemplate(document, ticker, error)) {
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

        StratumSingleWork *mainWork = isFirstBackend ? work : secondSequence.back().get();
        StratumSingleWork *extraWork = isFirstBackend ? secondSequence.back().get() : work;
        newMergedWork(firstIdx, secondIdx, mainWork, extraWork, miningConfig);
      }
    }

    return true;
  }

  bool isDuplicate(StratumWork *work, const typename X::Proto::BlockHashTy &shareHash) {
    bool duplicate = false;
    for (size_t i = 0, ie = work->backendsNum(); i != ie; ++i) {
      size_t backendId = work->backendId(i);
      if (backendId >= BackendsNum_)
        continue;

      duplicate |= !AcceptedShares_[backendId].insert(shareHash).second;
    }

    return duplicate;
  }


private:
  size_t BackendsNum_ = 0;
  StratumWork *CurrentWork_= nullptr;
  StratumWork *LastAcceptedWork_ = nullptr;
  std::unordered_map<PoolBackend*, size_t> BackendMap_;
  std::unique_ptr<bool[]> FirstBackends_;

  std::unique_ptr<CSingleWorkSequence[]> WorkStorage_;
  std::unique_ptr<CMergedWorkSequence[]> MergedWorkStorage_;
  std::unique_ptr<CAcceptedShareSet[]> AcceptedShares_;
  std::unordered_map<int64_t, StratumWork*> WorkIdMap_;

  int64_t lastStratumId = 0;

private: 
  StratumSingleWork *newSingleWork(PoolBackend *backend, size_t backendIdx, uint64_t uniqueId, const MiningConfig &miningCfg, const std::vector<uint8_t> &miningAddress, const std::string &coinbaseMessage, bool *newBlock) {
    lastStratumId = std::max(lastStratumId+1, static_cast<int64_t>(time(nullptr)));
    StratumSingleWork *work;
    if (FirstBackends_[backendIdx])
      work = new typename X::Stratum::Work(lastStratumId, uniqueId, backend, backendIdx, miningCfg, miningAddress, coinbaseMessage);
    else
      work = new typename X::Stratum::SecondWork(lastStratumId, uniqueId, backend, backendIdx, miningCfg, miningAddress, coinbaseMessage);

    *newBlock = false;
    CSingleWorkSequence &sequence = WorkStorage_[backendIdx];
    if (!sequence.empty() && sequence.back()->uniqueWorkId() != uniqueId) {
      // Cleanup main work
      for (const auto &w: sequence)
        WorkIdMap_.erase(w->stratumId());
      sequence.clear();
      AcceptedShares_[backendIdx].clear();
      *newBlock = true;
    }

    WorkIdMap_[lastStratumId] = work;
    LastAcceptedWork_ = work;
    sequence.emplace_back(work);

    // Remove old work
    while (sequence.size() > WorksetSizeLimit) {
      WorkIdMap_.erase(sequence.front()->stratumId());
      sequence.pop_front();
    }

    return work;
  }

  void newMergedWork(size_t firstIdx, size_t secondIdx, StratumSingleWork *first, StratumSingleWork *second, MiningConfig &miningCfg) {
    lastStratumId = std::max(lastStratumId+1, static_cast<int64_t>(time(nullptr)));
    StratumMergedWork *work = new typename X::Stratum::MergedWork(lastStratumId, first, second, miningCfg);
    WorkIdMap_[lastStratumId] = work;
    LastAcceptedWork_ = work;

    CMergedWorkSequence &sequence = MergedWorkStorage_[firstIdx * BackendsNum_ + secondIdx];
    sequence.emplace_back(work);

    // Cleanup
    while (!sequence.empty()) {
      if (sequence.front()->empty()) {
        WorkIdMap_.erase(sequence.front()->stratumId());
        sequence.pop_front();
      } else {
        break;
      }
    }
  }

private:
  static constexpr size_t WorksetSizeLimit = 3;
};



