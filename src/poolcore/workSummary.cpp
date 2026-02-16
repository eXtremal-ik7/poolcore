#include "poolcore/workSummary.h"
#include <algorithm>
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

void CWorkSummary::mergeScaled(const CWorkSummary &other, double fraction)
{
  SharesNum += static_cast<uint64_t>(std::round(other.SharesNum * fraction));
  UInt<256> scaledWork = other.SharesWork;
  scaledWork.mulfp(fraction);
  SharesWork += scaledWork;
  PrimePOWTarget = std::min(PrimePOWTarget, other.PrimePOWTarget);
  if (other.PrimePOWSharesNum.size() > PrimePOWSharesNum.size())
    PrimePOWSharesNum.resize(other.PrimePOWSharesNum.size());
  for (size_t i = 0; i < other.PrimePOWSharesNum.size(); i++)
    PrimePOWSharesNum[i] += static_cast<uint64_t>(std::round(other.PrimePOWSharesNum[i] * fraction));
}
