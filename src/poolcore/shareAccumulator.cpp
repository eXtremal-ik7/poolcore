#include "poolcore/shareAccumulator.h"
#include <algorithm>

void CShareAccumulator::initialize(std::chrono::seconds flushInterval, Timestamp now, bool accumulateUsers)
{
  FlushInterval_ = flushInterval;
  NextFlushTime_ = now + flushInterval;
  AccumulateUsers_ = accumulateUsers;
}

void CShareAccumulator::addShare(const std::string &userId, const std::string &workerId,
                                  const UInt<256> &workValue, Timestamp time,
                                  double chainLength, uint32_t primePOWTarget, bool isPrimePOW)
{
  BatchFirstTime_ = std::min(BatchFirstTime_, time);
  BatchLastTime_ = std::max(BatchLastTime_, time);

  // Worker accumulation
  auto &w = Workers_[{userId, workerId}];
  w.SharesNum++;
  w.SharesWork += workValue;

  if (isPrimePOW) {
    w.PrimePOWTarget = std::min(w.PrimePOWTarget, primePOWTarget);
    unsigned idx = std::min(static_cast<unsigned>(chainLength), 1024u);
    if (idx >= w.PrimePOWSharesNum.size())
      w.PrimePOWSharesNum.resize(idx + 1);
    w.PrimePOWSharesNum[idx]++;
  }

  // User accumulation
  if (AccumulateUsers_) {
    auto &u = Users_[userId];
    u.AcceptedWork += workValue;
    u.SharesNum++;
  }
}

CWorkSummaryBatch CShareAccumulator::takeWorkerBatch()
{
  CWorkSummaryBatch batch;
  batch.Time = {BatchFirstTime_, BatchLastTime_};
  batch.Entries.reserve(Workers_.size());
  for (auto &[key, w] : Workers_) {
    CWorkSummaryEntry entry;
    entry.UserId = key.first;
    entry.WorkerId = key.second;
    entry.Data.SharesNum = w.SharesNum;
    entry.Data.SharesWork = w.SharesWork;
    entry.Data.PrimePOWTarget = w.PrimePOWTarget;
    entry.Data.PrimePOWSharesNum = std::move(w.PrimePOWSharesNum);
    batch.Entries.emplace_back(std::move(entry));
  }
  Workers_.clear();
  BatchFirstTime_ = Timestamp(std::chrono::milliseconds::max());
  BatchLastTime_ = Timestamp();
  return batch;
}

CUserWorkSummaryBatch CShareAccumulator::takeUserBatch()
{
  CUserWorkSummaryBatch batch;
  batch.Time = {BatchFirstTime_, BatchLastTime_};
  batch.Entries.reserve(Users_.size());
  for (auto &[userId, u] : Users_) {
    CUserWorkSummary entry;
    entry.UserId = userId;
    entry.AcceptedWork = u.AcceptedWork;
    entry.SharesNum = u.SharesNum;
    entry.Time = BatchLastTime_;
    batch.Entries.emplace_back(std::move(entry));
  }
  Users_.clear();
  // Note: BatchFirstTime_/BatchLastTime_ reset in takeWorkerBatch
  return batch;
}

bool CShareAccumulator::empty() const
{
  return Workers_.empty();
}
