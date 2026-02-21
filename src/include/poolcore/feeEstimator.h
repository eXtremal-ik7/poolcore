#pragma once

#include "poolCore.h"
#include "poolcommon/periodicTimer.h"
#include "poolcommon/uint.h"
#include "asyncio/asyncio.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

class CNetworkClientDispatcher;

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

class CFeeEstimationService {

public:
  CFeeEstimationService(asyncBase *base, CNetworkClientDispatcher &dispatcher, const CCoinInfo &coinInfo);
  void start();
  void stop();
  void onNewBlock(int64_t height);
  const UInt<384> &averageFee() const { return AverageFee_; }

private:
  asyncBase *Base_;
  CNetworkClientDispatcher &Dispatcher_;
  CCoinInfo CoinInfo_;
  CFeeEstimator Estimator_;
  int64_t LastBlockHeight_ = 0;
  int64_t PendingHeight_ = 0;
  CPeriodicTimer UpdateTimer_;
  UInt<384> AverageFee_;
  bool Started_ = false;
  bool Supported_ = true;
};
