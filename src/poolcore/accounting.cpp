#include "poolcore/accounting.h"
#include "poolcore/accountingState.h"
#include "poolcore/feeEstimator.h"
#include "poolcommon/debug.h"
#include "poolcommon/mergeSorted.h"
#include "poolcommon/utils.h"
#include "poolcore/priceFetcher.h"
#include "loguru.hpp"
#include <cstring>
#include <math.h>
#include <thread>

namespace {
const std::string defaultFeePlan = "default";
const std::vector<::UserFeePair> emptyFeeRecord;
} // namespace

void AccountingDb::printRecentStatistic()
{
  if (State_.RecentStats.empty()) {
    LOG_F(INFO, "[%s] Recent statistic: empty", CoinInfo_.Name.c_str());
    return;
  }

  LOG_F(INFO, "[%s] Recent statistic:", CoinInfo_.Name.c_str());
  for (const auto &user: State_.RecentStats) {
    std::string line = user.UserId;
    line.append(": ");
    bool firstIter = true;
    for (const auto &stat: user.Recent) {
      if (!firstIter)
        line.append(", ");
      line.append(stat.SharesWork.getDecimal());
      firstIter = false;
    }

    LOG_F(INFO, " * %s", line.c_str());
  }
}

AccountingDb::AccountingDb(asyncBase *base,
                           const PoolBackendConfig &config,
                           const CCoinInfo &coinInfo,
                           UserManager &userMgr,
                           CNetworkClientDispatcher &clientDispatcher,
                           CPriceFetcher &priceFetcher) :
  Base_(base),
  _cfg(config),
  CoinInfo_(coinInfo),
  UserManager_(userMgr),
  ClientDispatcher_(clientDispatcher),
  PriceFetcher_(priceFetcher),
  State_(config.dbPath),
  RoundsDb_(config.dbPath / "accounting.rounds"),
  _balanceDb(config.dbPath / "balance.2"),
  _foundBlocksDb(config.dbPath / "foundBlocks.2"),
  _poolBalanceDb(config.dbPath / "poolBalance.2"),
  _payoutDb(config.dbPath / "payouts.2"),
  PPLNSPayoutsDb(config.dbPath / "pplns.payouts.2"),
  PPSPayoutsDb(config.dbPath / "pps.payouts"),
  PPSHistoryDb_(config.dbPath / "pps.history"),
  ShareLog_(config.dbPath / "accounting.worklog", coinInfo.Name, config.ShareLogFileSizeLimit),
  UserStatsAcc_("accounting.userstats", config.StatisticUserGridInterval, config.AccountingPPLNSWindow * 2),
  ShareLogFlushTimer_(base),
  UserStatsFlushTimer_(base),
  PPSPayoutTimer_(base),
  InstantPayoutTimer_(base),
  StateFlushTimer_(base),
  TaskHandler_(this, base),
  PayoutProcessor_(base,
                   config,
                   coinInfo,
                   clientDispatcher,
                   State_,
                   _payoutDb,
                   _balanceDb,
                   _poolBalanceDb,
                   _balanceMap,
                   UserSettings_),
  Api_(base,
       config,
       coinInfo,
       userMgr,
       clientDispatcher,
       PayoutProcessor_,
       State_,
       InstantPayoutTimer_,
       _foundBlocksDb,
       PPLNSPayoutsDb,
       PPSPayoutsDb,
       PPSHistoryDb_,
       _balanceMap)
{

  // Start loading user data from UserManager in parallel with local DB loading
  std::thread settingsThread([this]() {
    UserManager_.fillUserCoinSettings(CoinInfo_.Name, UserSettings_);
    LOG_F(INFO, "AccountingDb: loaded %zu user settings for %s", UserSettings_.size(), CoinInfo_.Name.c_str());
  });

  std::thread feePlanIdsThread([this]() {
    UserManager_.fillUserFeePlanIds(UserFeePlanIds_);
    LOG_F(INFO, "AccountingDb: loaded %zu user fee plan assignments for %s", UserFeePlanIds_.size(), CoinInfo_.Name.c_str());
  });

  std::thread feePlansThread([this]() {
    for (unsigned m = 0; m < static_cast<unsigned>(EMiningMode::Count); ++m) {
      EMiningMode mode = static_cast<EMiningMode>(m);
      auto records = UserManager_.getAllFeeRecords(mode, CoinInfo_.Name);
      for (auto &[feePlanId, feeList] : records)
        FeePlanCache_[{feePlanId, mode}] = std::move(feeList);
    }
    LOG_F(INFO, "AccountingDb: loaded %zu fee plan entries for %s", FeePlanCache_.size(), CoinInfo_.Name.c_str());
  });

  UserStatsAcc_.load(config.dbPath, coinInfo.Name);
  State_.load(coinInfo);
  LOG_F(INFO, "loaded %u payouts from db", static_cast<unsigned>(State_.PayoutQueue.size()));

  LOG_F(INFO, "loaded %zu active rounds from state", State_.ActiveRounds.size());

  {
    std::unique_ptr<rocksdbBase::IteratorType> It(_balanceDb.iterator());
    It->seekFirst();
    for (; It->valid(); It->next()) {
      UserBalanceRecord ub;
      RawData data = It->value();
      if (ub.deserializeValue(data.data, data.size))
        _balanceMap[ub.Login] = ub;
    }

    LOG_F(INFO, "loaded %u user balance data from db", (unsigned)_balanceMap.size());
  }

  // Wait for user data loading threads before replay
  // (replay needs UserSettings_, UserFeePlanIds_, FeePlanCache_ for PPS mode handling)
  settingsThread.join();
  feePlanIdsThread.join();
  feePlansThread.join();

  {
    uint64_t replayFrom = std::min(State_.lastAcceptedMsgId(), UserStatsAcc_.lastSavedMsgId());
    ShareLog_.replay([this, replayFrom](uint64_t messageId, const CUserWorkSummaryBatch &batch) {
      if (messageId <= replayFrom)
        return;
      auto processed = processWorkSummaryBatch(batch);
      State_.applyBatch(messageId, processed.AccountingBatch);
      UserStatsAcc_.addBaseWorkBatch(messageId, processed.StatsBatch);
    });
  }

  printRecentStatistic();
  if (!State_.CurrentScores.empty()) {
    LOG_F(INFO, "[%s] current scores:", CoinInfo_.Name.c_str());
    for (const auto &It: State_.CurrentScores)
      LOG_F(INFO, " * %s: %s", It.first.c_str(), It.second.getDecimal().c_str());
  } else {
    LOG_F(INFO, "[%s] current scores is empty", CoinInfo_.Name.c_str());
  }
  // Flush replayed data immediately so AccumulationInterval_ is reset.
  // Otherwise, if the pool was down for a long time, the first live share
  // would stretch the interval and corrupt grid distribution.
  UserStatsAcc_.flush(Timestamp::now(), _cfg.dbPath, nullptr);

  ShareLog_.startLogging(lastKnownShareId() + 1);
}


void AccountingDb::start()
{
  TaskHandler_.start();

  ShareLogFlushTimer_.start([this]() {
    ShareLog_.flush();
    ShareLog_.cleanupOldFiles(lastAggregatedShareId());
  }, _cfg.ShareLogFlushInterval, false, true);

  StateFlushTimer_.start([this]() {
    auto batch = CAccountingState::batch();
    State_.addMutableState(batch);
    State_.flushState(batch);
  }, std::chrono::minutes(1), false, true);

  UserStatsFlushTimer_.start([this]() {
    UserStatsAcc_.flush(Timestamp::now(), _cfg.dbPath, nullptr);
  }, _cfg.StatisticUserFlushInterval, false, true);

  PPSPayoutTimer_.start([this]() {
    ppsPayout();
  }, _cfg.PPSPayoutInterval);

  auto payoutConfig = backendSettings().PayoutConfig;
  InstantPayoutTimer_.start([this]() {
    PayoutProcessor_.makePayout();
  }, std::chrono::duration_cast<std::chrono::microseconds>(payoutConfig.InstantPayoutInterval));
  if (!payoutConfig.InstantPayoutsEnabled)
    InstantPayoutTimer_.pause();
}

void AccountingDb::stop()
{
  const char *coin = CoinInfo_.Name.c_str();
  TaskHandler_.stop(coin, "accounting: task handler");
  ShareLogFlushTimer_.stop();
  StateFlushTimer_.stop();
  UserStatsFlushTimer_.stop();
  PPSPayoutTimer_.stop();
  InstantPayoutTimer_.stop();
  ShareLogFlushTimer_.wait(coin, "accounting: share log flush");
  StateFlushTimer_.wait(coin, "accounting: state flush");
  UserStatsFlushTimer_.wait(coin, "accounting: user stats flush");
  PPSPayoutTimer_.wait(coin, "accounting: pps payout");
  InstantPayoutTimer_.wait(coin, "accounting: instant payout");
}

void AccountingDb::ppsPayout()
{
  if (State_.PPSPendingBalance.empty())
    return;

  LOG_F(INFO, "[%s] PPS payout: %zu users", CoinInfo_.Name.c_str(), State_.PPSPendingBalance.size());

  Timestamp now = Timestamp::now();
  Timestamp intervalBegin = now - _cfg.PPSPayoutInterval;

  CRewardParams rewardParams;
  rewardParams.IntervalBegin = intervalBegin;
  rewardParams.IntervalEnd = now;
  rewardParams.RateToBTC = PriceFetcher_.getPrice(CoinInfo_.Name);
  rewardParams.RateBTCToUSD = PriceFetcher_.getBtcUsd();

  kvdb<rocksdbBase>::Batch balanceBatch;
  kvdb<rocksdbBase>::Batch payoutBatch;
  bool payoutQueued = false;
  UInt<384> totalPaid = UInt<384>::zero();
  for (auto &[userId, value] : State_.PPSPendingBalance) {
    totalPaid += value;
    if (applyReward(userId, value, EMiningMode::PPS, rewardParams, balanceBatch, payoutBatch))
      payoutQueued = true;
    LOG_F(INFO, " * %s: +%s", userId.c_str(), FormatMoneyFull(value, CoinInfo_.FractionalPartSize).c_str());
  }

  LOG_F(INFO,
        "[%s] PPS payout total: %s",
        CoinInfo_.Name.c_str(),
        FormatMoney(totalPaid, CoinInfo_.FractionalPartSize).c_str());

  if (!balanceBatch.empty())
    _balanceDb.writeBatch(balanceBatch);
  if (!payoutBatch.empty())
    PPSPayoutsDb.writeBatch(payoutBatch);

  State_.PPSPendingBalance.clear();
  State_.PPSState.Time = now;
  auto batch = CAccountingState::batch();
  if (payoutQueued)
    State_.addPayoutQueue(batch);
  State_.addMutableState(batch);
  State_.flushState(batch);
  PPSHistoryDb_.put(State_.PPSState);
}

void AccountingDb::applyPPSCorrection(MiningRound &R)
{
  auto ppsMetaIt = std::find_if(R.UserShares.begin(), R.UserShares.end(),
    [](const UserShareValue &s) { return s.UserId == ppsMetaUserId(); });
  if (ppsMetaIt == R.UserShares.end())
    return;

  if (R.TotalShareValue.isZero() || R.AvailableCoins.isZero())
    return;

  double ppsFraction = UInt<256>::fpdiv(ppsMetaIt->ShareValue, R.TotalShareValue);
  UInt<384> ppsShare = R.AvailableCoins / R.TotalShareValue;
  ppsShare *= ppsMetaIt->ShareValue;

  R.AvailableCoins -= ppsShare;
  R.TotalShareValue -= ppsMetaIt->ShareValue;
  R.UserShares.erase(ppsMetaIt);

  R.PPSValue = ppsShare;
  R.PPSBlockPart = ppsFraction;

  State_.PPSState.Balance += ppsShare;
  State_.PPSState.ReferenceBalance += ppsShare;
  State_.PPSState.TotalBlocksFound += ppsFraction;
  State_.PPSState.updateMinMax(Timestamp::now());
  LOG_F(INFO,
        " * PPS correction: %s (%.3f of block, remaining for PPLNS: %s)",
        FormatMoney(ppsShare, CoinInfo_.FractionalPartSize).c_str(),
        ppsFraction,
        FormatMoney(R.AvailableCoins, CoinInfo_.FractionalPartSize).c_str());
}

void AccountingDb::calculatePPLNSPayments(MiningRound &R)
{
  R.Payouts.clear();

  if (R.TotalShareValue.isZero()) {
    LOG_F(WARNING, "Block found but no PPLNS shares, skipping payouts");
    return;
  }

  UInt<384> totalPayout = UInt<384>::zero();
  std::vector<CUserPayout> payouts;
  std::map<std::string, UInt<384>> feePayouts;
  UInt<384> minShareCost = R.AvailableCoins / R.TotalShareValue;
  for (const auto &record: R.UserShares) {
    // Calculate payout
    UInt<384> payoutValue = minShareCost * record.ShareValue;
    totalPayout += payoutValue;

    // get fee plan for user
    auto feePlanIt = UserFeePlanIds_.find(record.UserId);
    const std::string &feePlanId = feePlanIt != UserFeePlanIds_.end() ? feePlanIt->second : defaultFeePlan;
    auto cacheIt = FeePlanCache_.find({feePlanId, EMiningMode::PPLNS});
    const std::vector<::UserFeePair> &feeRecord = cacheIt != FeePlanCache_.end() ? cacheIt->second : emptyFeeRecord;

    UInt<384> feeValuesSum = UInt<384>::zero();
    std::vector<UInt<384>> feeValues;
    for (const auto &poolFeeRecord: feeRecord) {
      UInt<384> value = payoutValue;
        // mulfp uses 53-bit mantissa; ±1 satoshi possible for amounts > 2^53
        value.mulfp(poolFeeRecord.Percentage / 100.0);
      feeValues.push_back(value);
      feeValuesSum += value;
    }

    std::string debugString;
    if (feeValuesSum <= payoutValue) {
      for (size_t i = 0, ie = feeRecord.size(); i != ie; ++i) {
        debugString.append(feeRecord[i].UserId);
        debugString.push_back('(');
        debugString.append(FormatMoney(feeValues[i], CoinInfo_.FractionalPartSize));
        debugString.append(") ");
        feePayouts[feeRecord[i].UserId] += feeValues[i];
      }

      payoutValue -= feeValuesSum;
    } else {
      feeValuesSum = UInt<384>::zero();
      feeValues.clear();
      debugString = "NONE";
      LOG_F(ERROR, "   * user %s: fee over 100%% can't be applied", record.UserId.c_str());
    }

    payouts.emplace_back(record.UserId, payoutValue, payoutValue, record.IncomingWork);
    LOG_F(INFO, " * %s %s -> %sremaining %s", record.UserId.c_str(), FormatMoney(payoutValue+feeValuesSum, CoinInfo_.FractionalPartSize).c_str(), debugString.c_str(), FormatMoney(payoutValue, CoinInfo_.FractionalPartSize).c_str());
  }

  mergeSorted(payouts.begin(), payouts.end(), feePayouts.begin(), feePayouts.end(),
    [](const CUserPayout &l, const std::pair<std::string, UInt<384>> &r) { return l.UserId < r.first; },
    [](const std::pair<std::string, UInt<384>> &l, const CUserPayout &r) { return l.first < r.UserId; },
    [&R](const CUserPayout &record) {
      if (!record.Value.isZero())
        R.Payouts.emplace_back(record);
    }, [&R](const std::pair<std::string, UInt<384>> &fee) {
      if (!fee.second.isZero())
        R.Payouts.emplace_back(fee.first, fee.second, UInt<384>::zero(), UInt<256>::zero());
    }, [&R](const CUserPayout &record, const std::pair<std::string, UInt<384>> &fee) {
    if (!(record.Value + fee.second).isZero())
        R.Payouts.emplace_back(record.UserId, record.Value + fee.second, record.Value, record.AcceptedWork);
    });

  // Correct payouts for use all available coins
  if (!R.Payouts.empty()) {
    UInt<384> diff;
    UInt<384> div;
    bool needSubtract = totalPayout > R.AvailableCoins;

    if (needSubtract) {
      diff = totalPayout - R.AvailableCoins;
    } else {
      diff = R.AvailableCoins - totalPayout;
    }
    uint64_t mod = diff.divmod64(R.Payouts.size(), &div);

    totalPayout = UInt<384>();
    uint64_t i = 0;
    for (auto I = R.Payouts.begin(), IE = R.Payouts.end(); I != IE; ++I, ++i) {
      if (needSubtract) {
        I->Value -= div;
        if (i < mod)
          I->Value -= 1u;
      } else {
        I->Value += div;
        if (i < mod)
          I->Value += 1u;
      }
      totalPayout += I->Value;
      LOG_F(INFO, "   * %s: payout: %s", I->UserId.c_str(), FormatMoney(I->Value, CoinInfo_.FractionalPartSize).c_str());
    }

    LOG_F(INFO, " * total payout (after correct): %s", FormatMoney(totalPayout, CoinInfo_.FractionalPartSize).c_str());
  }
}

void AccountingDb::onUserWorkSummary(const CUserWorkSummaryBatch &batch)
{
  if (batch.Time.TimeBegin > batch.Time.TimeEnd ||
      (batch.Time.TimeEnd - batch.Time.TimeBegin) > MaxBatchTimeInterval) {
    LOG_F(ERROR,
          "AccountingDb::onUserWorkSummary: invalid batch time [%" PRId64 ", %" PRId64 "], %zu entries dropped",
          batch.Time.TimeBegin.count(),
          batch.Time.TimeEnd.count(),
          batch.Entries.size());
    return;
  }

  uint64_t messageId = ShareLog_.addMessage(batch);
  auto processed = processWorkSummaryBatch(batch);
  State_.applyBatch(messageId, processed.AccountingBatch);
  UserStatsAcc_.addBaseWorkBatch(messageId, processed.StatsBatch);
}

CProcessedWorkSummary AccountingDb::processWorkSummaryBatch(const CUserWorkSummaryBatch &batch)
{
  CProcessedWorkSummary result;
  result.StatsBatch.Time = batch.Time;

  // Special meta-user for aggregating PPS shares into PPLNS scoring
  CUserWorkSummary ppsMeta;
  ppsMeta.UserId = ppsMetaUserId();

  auto settings = State_.BackendSettings.load(std::memory_order_relaxed);
  bool ppsEnabled = settings.PPSConfig.Enabled;

  // Determine LastBaseBlockReward from batch (all entries share the same value)
  UInt<384> lastBaseBlockReward = State_.PPSState.LastBaseBlockReward;
  for (const auto &entry : batch.Entries) {
    if (!entry.BaseBlockReward.isZero()) {
      lastBaseBlockReward = entry.BaseBlockReward;
      break;
    }
  }
  result.AccountingBatch.LastBaseBlockReward = lastBaseBlockReward;

  // Compute saturation coefficient once per batch
  double saturateCoeff = 1.0;
  double ppsPoolFee = settings.PPSConfig.PoolFee;
  UInt<384> averageTxFee = State_.PPSState.LastAverageTxFee;
  if (ppsEnabled) {
    double currentBalanceInBlocks =
      CPPSState::balanceInBlocks(State_.PPSState.ReferenceBalance, lastBaseBlockReward);
    saturateCoeff = settings.PPSConfig.saturateCoeff(currentBalanceInBlocks);
    if (FeeEstimationService_)
      averageTxFee = FeeEstimationService_->averageFee();
  }
  result.AccountingBatch.LastSaturateCoeff = saturateCoeff;
  result.AccountingBatch.LastAverageTxFee = averageTxFee;

  for (const auto &entry : batch.Entries) {
    EMiningMode mode = EMiningMode::PPLNS;
    if (ppsEnabled) {
      auto settingsIt = UserSettings_.find(entry.UserId);
      if (settingsIt != UserSettings_.end())
        mode = settingsIt->second.Mining.MiningMode;
    }

    if (mode == EMiningMode::PPLNS) {
      // PPLNS: add shares to scores and user stats
      result.AccountingBatch.PPLNSScores.emplace_back(entry.UserId, entry.AcceptedWork);
      result.StatsBatch.Entries.push_back({entry.UserId, entry.AcceptedWork, entry.SharesNum, {}, {}});
    } else {
      // PPS: aggregate shares for meta-user, calculate reward
      ppsMeta.SharesNum += entry.SharesNum;
      ppsMeta.AcceptedWork += entry.AcceptedWork;

      if (!entry.ExpectedWork.isZero()) {
        // Fixed-point 128.256: high 128 bits = integer satoshi, low 256 bits = fractional
        UInt<384> batchCost = lastBaseBlockReward + averageTxFee;
        batchCost /= entry.ExpectedWork;
        batchCost *= entry.AcceptedWork;
        batchCost.mulfp(saturateCoeff);
        result.AccountingBatch.PPSReferenceCost += batchCost;

        // Pool fee (stays in PPS balance, not distributed)
        UInt<384> poolFeeValue = batchCost;
        poolFeeValue.mulfp(ppsPoolFee / 100.0);

        // User fee plan (distributed to fee recipients)
        auto feePlanIt = UserFeePlanIds_.find(entry.UserId);
        const std::string &feePlanId =
          feePlanIt != UserFeePlanIds_.end() ? feePlanIt->second : defaultFeePlan;
        auto cacheIt = FeePlanCache_.find({feePlanId, EMiningMode::PPS});
        const auto &feeRecord =
          cacheIt != FeePlanCache_.end() ? cacheIt->second : emptyFeeRecord;

        UInt<384> userFeeSum = UInt<384>::zero();
        std::vector<UInt<384>> feeValues;
        for (const auto &fee : feeRecord) {
          UInt<384> feeValue = batchCost;
          feeValue.mulfp(fee.Percentage / 100.0);
          feeValues.push_back(feeValue);
          userFeeSum += feeValue;
        }

        UInt<384> totalFeeSum = poolFeeValue + userFeeSum;
        if (totalFeeSum <= batchCost) {
          for (size_t i = 0, ie = feeRecord.size(); i != ie; ++i)
            result.AccountingBatch.PPSBalances.emplace_back(feeRecord[i].UserId, feeValues[i]);
          result.AccountingBatch.PPSBalances.emplace_back(entry.UserId, batchCost - totalFeeSum);
        } else {
          double userFeePct = 0.0;
          for (const auto &fee : feeRecord)
            userFeePct += fee.Percentage;
          LOG_F(ERROR,
            "PPS user %s: fee sum (pool %.2f%% + user %.2f%%) exceeds 100%%, fees not applied",
            entry.UserId.c_str(),
            ppsPoolFee,
            userFeePct);
          result.AccountingBatch.PPSBalances.emplace_back(entry.UserId, batchCost);
        }
      }
    }
  }

  // Add aggregated PPS shares to scores and user stats under meta-user
  if (ppsMeta.SharesNum) {
    result.AccountingBatch.PPLNSScores.emplace_back(ppsMeta.UserId, ppsMeta.AcceptedWork);
    result.StatsBatch.Entries.push_back(
      {ppsMeta.UserId, ppsMeta.AcceptedWork, ppsMeta.SharesNum, {}, {}});
  }

  return result;
}

void AccountingDb::onUserSettingsUpdate(const UserSettingsRecord &settings)
{
  UserSettings_[settings.Login] = settings;
}

void AccountingDb::onFeePlanUpdate(const std::string &feePlanId, EMiningMode mode, const std::vector<::UserFeePair> &feeRecord)
{
  FeePlanCache_[{feePlanId, mode}] = feeRecord;
  LOG_F(INFO, "AccountingDb %s: fee plan '%s' updated for mode %s, %zu entries",
    CoinInfo_.Name.c_str(), feePlanId.c_str(), miningModeName(mode), feeRecord.size());
}

void AccountingDb::onFeePlanDelete(const std::string &feePlanId)
{
  for (unsigned m = 0; m < static_cast<unsigned>(EMiningMode::Count); ++m)
    FeePlanCache_.erase({feePlanId, static_cast<EMiningMode>(m)});
  LOG_F(INFO, "AccountingDb %s: fee plan '%s' deleted", CoinInfo_.Name.c_str(), feePlanId.c_str());
}

void AccountingDb::onUserFeePlanChange(const std::string &login, const std::string &feePlanId)
{
  UserFeePlanIds_[login] = feePlanId;
  LOG_F(INFO, "AccountingDb %s: user '%s' fee plan changed to '%s'", CoinInfo_.Name.c_str(), login.c_str(), feePlanId.c_str());
}

void AccountingDb::onBlockFound(const CBlockFoundData &block)
{
  UInt<256> accumulatedWork = UInt<256>::zero();
  for (const auto &score: State_.CurrentScores)
    accumulatedWork += score.second;

  {
    // save to database
    FoundBlockRecord blk;
    blk.Height = block.Height;
    blk.Hash = block.Hash;
    blk.Time = block.Time;
    blk.AvailableCoins = block.GeneratedCoins;
    blk.FoundBy = block.UserId;
    blk.ExpectedWork = block.ExpectedWork;
    blk.AccumulatedWork = accumulatedWork;
    if (hasDeferredReward())
      blk.PublicHash = "?";
    _foundBlocksDb.put(blk);
  }

  State_.ActiveRounds.emplace_back();
  MiningRound &R = State_.ActiveRounds.back();

  LOG_F(INFO, " * block height: %u, hash: %s, value: %s", (unsigned)block.Height, block.Hash.c_str(), FormatMoney(block.GeneratedCoins, CoinInfo_.FractionalPartSize).c_str());

  R.Height = block.Height;
  R.BlockHash = block.Hash;
  R.EndTime = block.Time;
  R.FoundBy = block.UserId;
  R.ExpectedWork = block.ExpectedWork;
  R.AccumulatedWork = accumulatedWork;
  R.TotalShareValue = UInt<256>::zero();
  R.PrimePOWTarget = block.PrimePOWTarget;


  R.StartTime = State_.CurrentRoundStartTime;

  // Merge shares for current block with older shares (PPLNS)
  // RecentStats is a snapshot from the previous block-found event; CurrentScores covers
  // all work since then (replayed from ShareLog after restart). Together they always span
  // the full PPLNS window — no re-export from UserStatsAcc is needed here.
  {
    Timestamp acceptSharesTime = block.Time - _cfg.AccountingPPLNSWindow;
    mergeSorted(State_.RecentStats.begin(), State_.RecentStats.end(), State_.CurrentScores.begin(), State_.CurrentScores.end(),
      [](const CStatsExportData &stats, const std::pair<std::string, UInt<256>> &scores) { return stats.UserId < scores.first; },
      [](const std::pair<std::string, UInt<256>> &scores, const CStatsExportData &stats) { return scores.first < stats.UserId; },
      [&](const CStatsExportData &stats) {
        // User disconnected recently, no new shares
        UInt<256> shareValue = stats.recentShareValue(acceptSharesTime);
        if (shareValue.nonZero()) {
          R.UserShares.emplace_back(stats.UserId, shareValue, UInt<256>::zero());
        }
      }, [&](const std::pair<std::string, UInt<256>> &scores) {
        // User joined recently, no extra shares in statistic
        R.UserShares.emplace_back(scores.first, scores.second, scores.second);
      }, [&](const CStatsExportData &stats, const std::pair<std::string, UInt<256>> &scores) {
        // Need merge new shares and recent share statistics
        R.UserShares.emplace_back(stats.UserId, scores.second + stats.recentShareValue(acceptSharesTime), scores.second);
      });
  }

  // Calculate total share value
  for (const auto &element: R.UserShares)
    R.TotalShareValue += element.ShareValue;

  R.AvailableCoins = block.GeneratedCoins;
  if (!hasDeferredReward()) {
    applyPPSCorrection(R);
    calculatePPLNSPayments(R);
  }

  // Query statistics
  UserStatsAcc_.exportRecentStats(_cfg.AccountingPPLNSWindow, State_.RecentStats);
  printRecentStatistic();

  // Reset aggregated data
  State_.CurrentScores.clear();
  State_.CurrentRoundStartTime = R.EndTime;

  // Save state to db
  State_.PPSState.Time = Timestamp::now();
  auto batch = CAccountingState::batch();
  batch.put(R);
  State_.addMutableState(batch);
  State_.addRoundState(batch);
  State_.flushState(batch);
  PPSHistoryDb_.put(State_.PPSState);
}

AccountingDb::ERoundConfirmationResult AccountingDb::processRoundConfirmation(MiningRound &R,
                                                                              int64_t confirmations,
                                                                              const std::string &hash,
                                                                              rocksdbBase::CBatch &stateBatch)
{
  if (confirmations == -1) {
    LOG_F(INFO, "block %" PRIu64 "/%s marked as orphan, can't do any payout", R.Height, hash.c_str());
    if (R.PPSValue.nonZero()) {
      State_.PPSState.Balance -= R.PPSValue;
      State_.PPSState.ReferenceBalance -= R.PPSValue;
      State_.PPSState.TotalBlocksFound -= R.PPSBlockPart;
      State_.PPSState.OrphanBlocks += R.PPSBlockPart;
      State_.PPSState.updateMinMax(Timestamp::now());
      LOG_F(INFO,
            " * PPS correction reversed: %s (%.3f of block)",
            FormatMoney(R.PPSValue, CoinInfo_.FractionalPartSize).c_str(),
            R.PPSBlockPart);
    }

    RoundsDb_.put(R);
    stateBatch.deleteRow(R);
    return ERoundConfirmationResult::EOrphan;
  } else if (confirmations >= _cfg.RequiredConfirmations) {
    LOG_F(INFO, "Make payout for block %" PRIu64 "/%s", R.Height, R.BlockHash.c_str());
    if (hasDeferredReward()) {
      applyPPSCorrection(R);
      calculatePPLNSPayments(R);
    }

    CRewardParams rewardParams;
    rewardParams.RoundStartTime = R.StartTime;
    rewardParams.RoundEndTime = R.EndTime;
    rewardParams.BlockHash = R.BlockHash;
    rewardParams.BlockHeight = R.Height;
    rewardParams.RateToBTC = PriceFetcher_.getPrice(CoinInfo_.Name);
    rewardParams.RateBTCToUSD = PriceFetcher_.getBtcUsd();

    kvdb<rocksdbBase>::Batch balanceBatch;
    kvdb<rocksdbBase>::Batch payoutBatch;
    for (auto I = R.Payouts.begin(), IE = R.Payouts.end(); I != IE; ++I)
      applyReward(I->UserId, I->Value, EMiningMode::PPLNS, rewardParams, balanceBatch, payoutBatch);
    if (!balanceBatch.empty())
      _balanceDb.writeBatch(balanceBatch);
    if (!payoutBatch.empty())
      PPLNSPayoutsDb.writeBatch(payoutBatch);


    RoundsDb_.put(R);
    stateBatch.deleteRow(R);
    return ERoundConfirmationResult::EConfirmed;
  }

  return ERoundConfirmationResult::ENotConfirmed;
}

void AccountingDb::checkBlockConfirmations()
{
  auto &activeRounds = State_.ActiveRounds;
  if (activeRounds.empty())
    return;

  LOG_F(INFO, "Checking %zu blocks for confirmations...", activeRounds.size());

  std::vector<CNetworkClient::GetBlockConfirmationsQuery> confirmationsQuery;
  for (auto &round : activeRounds)
    confirmationsQuery.push_back({round.BlockHash, round.Height});

  if (!ClientDispatcher_.ioGetBlockConfirmations(Base_, _cfg.RequiredConfirmations, confirmationsQuery)) {
    LOG_F(ERROR, "ioGetBlockConfirmations api call failed");
    return;
  }

  auto batch = CAccountingState::batch();
  bool hasOrphans = false;
  bool hasPayouts = false;
  size_t i = 0;
  for (auto it = activeRounds.begin(); it != activeRounds.end(); i++) {
    auto result = processRoundConfirmation(*it, confirmationsQuery[i].Confirmations, confirmationsQuery[i].Hash, batch);
    hasOrphans |= result == ERoundConfirmationResult::EOrphan;
    hasPayouts |= result == ERoundConfirmationResult::EConfirmed;
    if (result != ERoundConfirmationResult::ENotConfirmed)
      it = activeRounds.erase(it);
    else
      ++it;
  }

  if (hasOrphans || hasPayouts) {
    State_.PPSState.Time = Timestamp::now();
    State_.addMutableState(batch);
    PPSHistoryDb_.put(State_.PPSState);
  }
  if (hasPayouts)
    State_.addPayoutQueue(batch);
  State_.flushState(batch);
}

void AccountingDb::checkBlockExtraInfo()
{
  auto &activeRounds = State_.ActiveRounds;
  if (activeRounds.empty())
    return;

  LOG_F(INFO, "Checking %zu blocks for extra info...", activeRounds.size());

  std::vector<CNetworkClient::GetBlockExtraInfoQuery> confirmationsQuery;
  for (auto &round : activeRounds)
    confirmationsQuery.emplace_back(round.BlockHash, round.Height, round.TxFee, round.AvailableCoins);

  if (!ClientDispatcher_.ioGetBlockExtraInfo(Base_, _cfg.RequiredConfirmations, confirmationsQuery)) {
    LOG_F(ERROR, "ioGetBlockExtraInfo api call failed");
    return;
  }

  auto batch = CAccountingState::batch();
  bool hasOrphans = false;
  bool hasPayouts = false;
  size_t i = 0;
  for (auto it = activeRounds.begin(); it != activeRounds.end(); i++) {
    MiningRound &R = *it;

    bool rewardChanged = R.AvailableCoins != confirmationsQuery[i].BlockReward;
    if (rewardChanged) {
      // Update found block database
      FoundBlockRecord blk;
      blk.Height = R.Height;
      blk.Hash = R.BlockHash;
      blk.Time = R.EndTime;
      blk.AvailableCoins = confirmationsQuery[i].BlockReward;
      blk.FoundBy = R.FoundBy;
      blk.ExpectedWork = R.ExpectedWork;
      blk.AccumulatedWork = R.AccumulatedWork;
      blk.PublicHash = confirmationsQuery[i].PublicHash;
      _foundBlocksDb.put(blk);

      // Update round with final reward (payments deferred to confirmation)
      R.AvailableCoins = confirmationsQuery[i].BlockReward;
      R.TxFee = confirmationsQuery[i].TxFee;
      batch.put(R);
    }

    auto result = processRoundConfirmation(R, confirmationsQuery[i].Confirmations, confirmationsQuery[i].Hash, batch);
    hasOrphans |= result == ERoundConfirmationResult::EOrphan;
    hasPayouts |= result == ERoundConfirmationResult::EConfirmed;
    if (result != ERoundConfirmationResult::ENotConfirmed)
      it = activeRounds.erase(it);
    else
      ++it;
  }

  if (hasOrphans || hasPayouts) {
    State_.PPSState.Time = Timestamp::now();
    State_.addMutableState(batch);
    PPSHistoryDb_.put(State_.PPSState);
  }
  if (hasPayouts)
    State_.addPayoutQueue(batch);
  State_.flushState(batch);
}

bool AccountingDb::applyReward(const std::string &address,
                               const UInt<384> &value,
                               EMiningMode mode,
                               const CRewardParams &rewardParams,
                               kvdb<rocksdbBase>::Batch &balanceBatch,
                               kvdb<rocksdbBase>::Batch &payoutBatch)
{
  bool result = false;
  auto It = _balanceMap.find(address);
  if (It == _balanceMap.end())
    It = _balanceMap.insert(It, std::make_pair(address, UserBalanceRecord(address, _cfg.DefaultPayoutThreshold)));

  UserBalanceRecord &balance = It->second;
  balance.Balance += value;

  if (mode == EMiningMode::PPS) {
    balance.PPSPaid += value;

    CPPSPayout record;
    record.Login = address;
    record.IntervalBegin = rewardParams.IntervalBegin;
    record.IntervalEnd = rewardParams.IntervalEnd;
    record.PayoutValue = value;
    record.RateToBTC = rewardParams.RateToBTC;
    record.RateBTCToUSD = rewardParams.RateBTCToUSD;
    payoutBatch.put(record);
  } else {
    CPPLNSPayout record;
    record.Login = address;
    record.RoundStartTime = rewardParams.RoundStartTime;
    record.BlockHash = rewardParams.BlockHash;
    record.BlockHeight = rewardParams.BlockHeight;
    record.RoundEndTime = rewardParams.RoundEndTime;
    record.PayoutValue = value;
    record.RateToBTC = rewardParams.RateToBTC;
    record.RateBTCToUSD = rewardParams.RateBTCToUSD;
    payoutBatch.put(record);
  }

  auto settingsIt = UserSettings_.find(balance.Login);
  UInt<384> nonQueuedBalance = balance.Balance - balance.Requested;
  if (!nonQueuedBalance.isNegative() &&
      settingsIt != UserSettings_.end() &&
      settingsIt->second.Payout.AutoPayout) {
    auto instantMinimalPayout = State_.BackendSettings.load(std::memory_order_relaxed).PayoutConfig.InstantMinimalPayout;
    auto userMinimalPayout = settingsIt->second.Payout.MinimalPayout;
    if (userMinimalPayout < instantMinimalPayout) {
      LOG_F(WARNING,
            "[%s] User %s has MinimalPayout below pool's InstantMinimalPayout, using pool minimum",
            CoinInfo_.Name.c_str(),
            balance.Login.c_str());
      userMinimalPayout = instantMinimalPayout;
    }

    if (nonQueuedBalance >= userMinimalPayout) {
      State_.PayoutQueue.push_back(PayoutDbRecord(address, nonQueuedBalance));
      balance.Requested += nonQueuedBalance;
      result = true;
    }
  }

  balanceBatch.put(balance);
  return result;
}

