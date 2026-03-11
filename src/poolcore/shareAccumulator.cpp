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
                                  double chainLength, uint32_t primePOWTarget, bool isPrimePOW,
                                  const std::optional<UInt<256>> &shareHash)
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

  // Best share tracking
  if (shareHash.has_value()) {
    // Hash-based: smaller hash = better share
    if (!BestShare_.Hash.has_value() || *shareHash < *BestShare_.Hash) {
      BestShare_.Hash = *shareHash;
      BestShare_.Time = time;
      // ShareDifficulty computed later in takeBatch()
    }
  } else if (isPrimePOW) {
    // XPM: higher chainLength = better share
    if (chainLength > BestShare_.ShareDifficulty) {
      BestShare_.ShareDifficulty = chainLength;
      BestShare_.Time = time;
    }
  }
}

CAccumulatorBatch CShareAccumulator::takeBatch(const UInt<256> &powLimit)
{
  CAccumulatorBatch result;
  TimeInterval time = {BatchFirstTime_, BatchLastTime_};

  // Workers
  result.Workers.Time = time;
  result.Workers.Entries.reserve(Workers_.size());
  for (auto &[key, w] : Workers_) {
    CWorkSummaryEntry entry;
    entry.UserId = key.first;
    entry.WorkerId = key.second;
    entry.Data.SharesNum = w.SharesNum;
    entry.Data.SharesWork = w.SharesWork;
    entry.Data.PrimePOWTarget = w.PrimePOWTarget;
    entry.Data.PrimePOWSharesNum = std::move(w.PrimePOWSharesNum);
    result.Workers.Entries.emplace_back(std::move(entry));
  }
  Workers_.clear();

  // Users
  result.Users.Time = time;
  result.Users.Entries.reserve(Users_.size());
  for (auto &[userId, u] : Users_) {
    CUserWorkSummary entry;
    entry.UserId = userId;
    entry.AcceptedWork = u.AcceptedWork;
    entry.SharesNum = u.SharesNum;
    entry.BaseBlockReward = BaseBlockReward_;
    entry.ExpectedWork = ExpectedWork_;
    result.Users.Entries.emplace_back(std::move(entry));
  }
  Users_.clear();

  // Best share
  if (BestShare_.Hash.has_value() && powLimit.nonZero())
    BestShare_.ShareDifficulty = UInt<256>::fpdiv(powLimit, *BestShare_.Hash);
  BestShare_.BlockDifficulty = BlockDifficulty_;
  BestShare_.ExpectedWork = ExpectedWork_;
  result.Users.BestShare = BestShare_;
  BestShare_.reset();

  // Reset
  BatchFirstTime_ = Timestamp(std::chrono::milliseconds::max());
  BatchLastTime_ = Timestamp();
  return result;
}

bool CShareAccumulator::empty() const
{
  return Workers_.empty();
}
