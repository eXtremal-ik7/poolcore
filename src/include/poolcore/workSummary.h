#ifndef __WORK_SUMMARY_H_
#define __WORK_SUMMARY_H_

#include "poolcommon/uint.h"
#include "poolcommon/timeTypes.h"
#include <cstdint>
#include <string>
#include <vector>

struct CWorkSummary {
  uint64_t SharesNum = 0;
  UInt<256> SharesWork = UInt<256>::zero();
  TimeInterval Time;
  uint32_t PrimePOWTarget = -1U;
  std::vector<uint64_t> PrimePOWSharesNum;

  void reset();
  // Note: Time is not merged — callers always merge elements with matching time intervals
  void merge(const CWorkSummary &other);
  CWorkSummary scaled(double fraction) const;
  std::vector<CWorkSummary> distributeToGrid(int64_t beginMs, int64_t endMs, int64_t gridIntervalMs) const;
};

struct CWorkSummaryEntry {
  enum { CurrentRecordVersion = 1 };

  std::string UserId;
  std::string WorkerId;
  CWorkSummary Data;
};

#endif //__WORK_SUMMARY_H_
