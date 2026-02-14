#ifndef __WORK_SUMMARY_H_
#define __WORK_SUMMARY_H_

#include "poolcommon/uint.h"
#include "poolcommon/timeTypes.h"
#include <cstdint>
#include <string>
#include <vector>

struct CWorkSummaryWithTime;

struct CWorkSummary {
  uint64_t SharesNum = 0;
  UInt<256> SharesWork = UInt<256>::zero();
  uint32_t PrimePOWTarget = -1U;
  std::vector<uint64_t> PrimePOWSharesNum;

  void reset();
  void merge(const CWorkSummary &other);
  CWorkSummary scaled(double fraction) const;
  std::vector<CWorkSummaryWithTime> distributeToGrid(int64_t beginMs, int64_t endMs, int64_t gridIntervalMs) const;
};

struct CWorkSummaryWithTime {
  TimeInterval Time;
  CWorkSummary Data;
};

struct CWorkSummaryEntry {
  enum { CurrentRecordVersion = 1 };

  std::string UserId;
  std::string WorkerId;
  CWorkSummary Data;
};

struct CWorkSummaryBatch {
  TimeInterval Time;
  std::vector<CWorkSummaryEntry> Entries;
};

#endif //__WORK_SUMMARY_H_
