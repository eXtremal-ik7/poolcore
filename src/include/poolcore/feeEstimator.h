#pragma once

#include <cstdint>
#include <deque>

class CFeeEstimator {

public:
  static constexpr int64_t WindowSeconds = 12 * 3600;
  static constexpr size_t MinBlocks = 60;

  struct FeeEntry {
    int64_t Time;
    int64_t TotalFee;
  };

public:
  void addBlock(int64_t time, int64_t totalFee);
  void trimWindow(int64_t latestTime);
  int64_t computeNormalizedAvgFee() const;
  size_t size() const { return Window_.size(); }
  bool empty() const { return Window_.empty(); }

private:
  std::deque<FeeEntry> Window_;
};
