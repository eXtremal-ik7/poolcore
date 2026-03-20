#include "poolcore/feeEstimator.h"
#include "poolcore/clientDispatcher.h"
#include "blockmaker/eth.h"

#include "poolcommon/utils.h"
#include "loguru.hpp"

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

// CFeeEstimationService

CFeeEstimationService::CFeeEstimationService(asyncBase *base, CNetworkClientDispatcher &dispatcher, const CCoinInfo &coinInfo)
    : Base_(base), Dispatcher_(dispatcher), CoinInfo_(coinInfo),
      Mode_(dispatcher.feeEstimationMode()), UpdateTimer_(base)
{
}

void CFeeEstimationService::start()
{
  if (Mode_ == EFeeEstimationMode::Unsupported) {
    CLOG_F(INFO, "{}: fee estimation not supported", CoinInfo_.Name);
    return;
  }

  Started_ = true;
  CLOG_F(INFO, "{}: fee estimation mode: {}",
         CoinInfo_.Name,
         Mode_ == EFeeEstimationMode::BlockTxFees ? "BlockTxFees" : "MiningInfoRpc");

  UpdateTimer_.start([this]() {
    int64_t height = PendingHeight_;
    if (height == 0)
      return;

    switch (Mode_) {
      case EFeeEstimationMode::BlockTxFees: updateBlockTxFees(); break;
      case EFeeEstimationMode::MiningInfoRpc: updateMiningInfo(); break;
      default: break;
    }
  });
}

void CFeeEstimationService::stop()
{
  if (!Started_)
    return;
  UpdateTimer_.stop();
  UpdateTimer_.wait(CoinInfo_.Name.c_str(), "fee estimation");
}

void CFeeEstimationService::onNewBlock(int64_t height)
{
  if (height <= PendingHeight_)
    return;
  PendingHeight_ = height;
  UpdateTimer_.activate();
}

void CFeeEstimationService::updateBlockTxFees()
{
  int64_t toHeight = PendingHeight_ - 1;
  int64_t fromHeight;
  if (LastBlockHeight_ == 0) {
    fromHeight = std::max<int64_t>(1, toHeight - 299);
  } else {
    fromHeight = LastBlockHeight_ + 1;
  }

  if (fromHeight > toHeight)
    return;

  std::vector<CNetworkClient::BlockTxFeeInfo> fees;
  if (!Dispatcher_.ioGetBlockTxFees(Base_, fromHeight, toHeight, fees)) {
    CLOG_F(WARNING, "{}: ioGetBlockTxFees failed", CoinInfo_.Name);
    return;
  }

  for (auto &entry : fees)
    Estimator_.addBlock(entry.Time, entry.TotalFee);
  if (!fees.empty())
    Estimator_.trimWindow(fees.back().Time);
  LastBlockHeight_ = toHeight;

  int64_t avgFee = Estimator_.computeNormalizedAvgFee();
  AverageFee_ = avgFee > 0 ? fromRational(static_cast<uint64_t>(avgFee)) : UInt<384>();
  CLOG_F(INFO, "{}: average block tx fee: {} ({} blocks in window)",
         CoinInfo_.Name,
         FormatMoney(AverageFee_, CoinInfo_.FractionalPartSize),
         Estimator_.size());
}

void CFeeEstimationService::updateMiningInfo()
{
  CNetworkClient::MiningInfo info;
  if (!Dispatcher_.ioGetMiningInfo(Base_, info)) {
    CLOG_F(WARNING, "{}: ioGetMiningInfo failed", CoinInfo_.Name);
    return;
  }
  CachedBaseReward_ = info.BaseBlockReward;
  if (!info.AverageTxFee.isZero())
    AverageFee_ = info.AverageTxFee;
  CLOG_F(INFO, "{}: mining info: base reward {}, avg fee {}",
         CoinInfo_.Name,
         FormatMoney(info.BaseBlockReward, CoinInfo_.FractionalPartSize),
         FormatMoney(info.AverageTxFee, CoinInfo_.FractionalPartSize));
}

UInt<384> CFeeEstimationService::estimatedBaseReward(int64_t height) const
{
  if (Mode_ == EFeeEstimationMode::MiningInfoRpc)
    return CachedBaseReward_;
  if (CoinInfo_.HasDeferredReward)
    return ETH::getConstBlockReward(CoinInfo_.Name, height);
  return UInt<384>::zero();
}
