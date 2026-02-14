#pragma once

#include "poolcore/workSummary.h"
#include "poolcore/backendData.h"
#include <chrono>
#include <map>

class CShareAccumulator {
public:
  void initialize(std::chrono::seconds flushInterval, Timestamp now, bool accumulateUsers = true);

  void addShare(const std::string &userId, const std::string &workerId,
                const UInt<256> &workValue, Timestamp time,
                double chainLength, uint32_t primePOWTarget, bool isPrimePOW);

  CWorkSummaryBatch takeWorkerBatch();
  CUserWorkSummaryBatch takeUserBatch();
  bool empty() const;
  bool shouldFlush(Timestamp now) const { return now >= NextFlushTime_; }
  void resetFlushTime(Timestamp now) { NextFlushTime_ = now + FlushInterval_; }

private:
  struct WorkerData {
    uint64_t SharesNum = 0;
    UInt<256> SharesWork = UInt<256>::zero();
    uint32_t PrimePOWTarget = -1U;
    std::vector<uint64_t> PrimePOWSharesNum;
  };

  struct UserData {
    UInt<256> AcceptedWork = UInt<256>::zero();
    uint64_t SharesNum = 0;
  };

  std::map<std::pair<std::string, std::string>, WorkerData> Workers_;
  std::map<std::string, UserData> Users_;
  Timestamp NextFlushTime_;
  std::chrono::seconds FlushInterval_ = std::chrono::seconds(6);
  bool AccumulateUsers_ = true;
  Timestamp BatchFirstTime_ = Timestamp(std::chrono::milliseconds::max());
  Timestamp BatchLastTime_;
};
