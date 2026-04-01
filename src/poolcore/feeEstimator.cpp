#include "poolcore/feeEstimator.h"

#include <algorithm>
#include <vector>

void CFeeEstimator::addBlock(int64_t time, int64_t totalFee)
{
  Window_.push_back({time, totalFee});
}

void CFeeEstimator::trimWindow(int64_t latestTime)
{
  int64_t cutoff = latestTime - WindowSeconds;
  while (Window_.size() > MinBlocks && Window_.front().Time < cutoff)
    Window_.pop_front();
}

int64_t CFeeEstimator::computeNormalizedAvgFee() const
{
  if (Window_.empty())
    return 0;

  std::vector<int64_t> fees;
  fees.reserve(Window_.size());
  for (auto &e : Window_)
    fees.push_back(e.TotalFee);

  std::sort(fees.begin(), fees.end());

  size_t trim = fees.size() / 10;
  size_t start = trim;
  size_t end = fees.size() - trim;
  if (start >= end) {
    start = 0;
    end = fees.size();
  }

  int64_t sum = 0;
  for (size_t i = start; i < end; i++)
    sum += fees[i];

  return sum / static_cast<int64_t>(end - start);
}
