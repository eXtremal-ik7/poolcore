#include "poolcore/workSummary.h"
#include <algorithm>
#include <cassert>
#include <cmath>

void CWorkSummary::reset()
{
  SharesNum = 0;
  SharesWork = UInt<256>::zero();
  PrimePOWTarget = -1U;
  PrimePOWSharesNum.clear();
}

void CWorkSummary::merge(const CWorkSummary &other)
{
  SharesNum += other.SharesNum;
  SharesWork += other.SharesWork;
  PrimePOWTarget = std::min(PrimePOWTarget, other.PrimePOWTarget);
  if (other.PrimePOWSharesNum.size() > PrimePOWSharesNum.size())
    PrimePOWSharesNum.resize(other.PrimePOWSharesNum.size());
  for (size_t i = 0; i < other.PrimePOWSharesNum.size(); i++)
    PrimePOWSharesNum[i] += other.PrimePOWSharesNum[i];
}

CWorkSummary CWorkSummary::scaled(double fraction) const
{
  CWorkSummary result;
  result.SharesNum = static_cast<uint64_t>(std::round(SharesNum * fraction));
  result.SharesWork = SharesWork;
  result.SharesWork.mulfp(fraction);
  result.PrimePOWTarget = PrimePOWTarget;
  result.PrimePOWSharesNum.resize(PrimePOWSharesNum.size());
  for (size_t i = 0; i < PrimePOWSharesNum.size(); i++)
    result.PrimePOWSharesNum[i] = static_cast<uint64_t>(std::round(PrimePOWSharesNum[i] * fraction));
  return result;
}

static int64_t alignUpToGrid(int64_t timeMs, int64_t gridIntervalMs)
{
  return ((timeMs + gridIntervalMs - 1) / gridIntervalMs) * gridIntervalMs;
}

std::vector<CWorkSummaryWithTime> CWorkSummary::distributeToGrid(int64_t beginMs, int64_t endMs, int64_t gridIntervalMs) const
{
  std::vector<CWorkSummaryWithTime> results;
  if (beginMs >= endMs || (SharesNum == 0 && SharesWork.isZero()))
    return results;

  int64_t totalMs = endMs - beginMs;
  int64_t firstGridEnd = alignUpToGrid(beginMs + 1, gridIntervalMs);
  int64_t lastGridEnd = alignUpToGrid(endMs, gridIntervalMs);

  uint64_t remainingShares = SharesNum;
  UInt<256> remainingWork = SharesWork;
  std::vector<uint64_t> remainingPrimePOW = PrimePOWSharesNum;

  for (int64_t gridEnd = firstGridEnd; gridEnd <= lastGridEnd; gridEnd += gridIntervalMs) {
    int64_t gridStart = gridEnd - gridIntervalMs;
    int64_t overlapBegin = std::max(beginMs, gridStart);
    int64_t overlapEnd = std::min(endMs, gridEnd);
    int64_t overlapMs = overlapEnd - overlapBegin;
    if (overlapMs <= 0)
      continue;

    CWorkSummaryWithTime cell;
    if (gridEnd >= lastGridEnd) {
      // Last cell gets remainder
      cell.Data.SharesNum = remainingShares;
      cell.Data.SharesWork = remainingWork;
      cell.Data.PrimePOWTarget = PrimePOWTarget;
      cell.Data.PrimePOWSharesNum = remainingPrimePOW;
    } else {
      double fraction = static_cast<double>(overlapMs) / static_cast<double>(totalMs);
      cell.Data = scaled(fraction);
      cell.Data.SharesNum = std::min(cell.Data.SharesNum, remainingShares);
      remainingShares -= cell.Data.SharesNum;
      if (cell.Data.SharesWork > remainingWork)
        cell.Data.SharesWork = remainingWork;
      remainingWork -= cell.Data.SharesWork;
      for (size_t i = 0; i < PrimePOWSharesNum.size(); i++) {
        cell.Data.PrimePOWSharesNum[i] = std::min(cell.Data.PrimePOWSharesNum[i], remainingPrimePOW[i]);
        remainingPrimePOW[i] -= cell.Data.PrimePOWSharesNum[i];
      }
    }

    cell.Time = TimeInterval(Timestamp(gridStart), Timestamp(gridEnd));
    results.push_back(cell);
  }

  return results;
}
