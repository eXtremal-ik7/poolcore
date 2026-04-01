#include "poolcore/accountingApi.h"
#include "poolcore/accountingState.h"
#include "poolcommon/timeTypes.h"
#include "loguru.hpp"
#include <cmath>
#include <algorithm>

void CAccumulatedPayoutEntry::merge(const UInt<384> &payoutValue, double rateToBTC, double rateBTCToUSD, unsigned fractionalPartSize)
{
  double rateScale = rateToBTC * std::pow(10.0, 8 - static_cast<int>(fractionalPartSize));
  Value += payoutValue;

  UInt<384> btcValue = payoutValue;
  btcValue.mulfp(rateScale);
  ValueBTC += btcValue;

  UInt<384> usdValue = payoutValue;
  usdValue.mulfp(rateScale * rateBTCToUSD);
  ValueUSD += usdValue;
}

void CAccumulatedPayoutEntry::mergeScaled(const UInt<384> &payoutValue, double rateToBTC, double rateBTCToUSD, double coeff, unsigned fractionalPartSize)
{
  double rateScale = rateToBTC * std::pow(10.0, 8 - static_cast<int>(fractionalPartSize));

  UInt<384> scaled = payoutValue;
  scaled.mulfp(coeff);
  Value += scaled;

  UInt<384> btcValue = scaled;
  btcValue.mulfp(rateScale);
  ValueBTC += btcValue;

  UInt<384> usdValue = scaled;
  usdValue.mulfp(rateScale * rateBTCToUSD);
  ValueUSD += usdValue;
}

CAccountingApi::CAccountingApi(asyncBase *base,
                               const PoolBackendConfig &cfg,
                               const CCoinInfo &coinInfo,
                               UserManager &userManager,
                               CNetworkClient &networkClient,
                               CPayoutProcessor &payoutProcessor,
                               CAccountingState &state,
                               CPeriodicTimer &instantPayoutTimer,
                               kvdb<rocksdbBase> &foundBlocksDb,
                               kvdb<rocksdbBase> &pplnsPayoutsDb,
                               kvdb<rocksdbBase> &ppsPayoutsDb,
                               kvdb<rocksdbBase> &ppsHistoryDb) :
  Base_(base),
  Cfg_(cfg),
  CoinInfo_(coinInfo),
  UserManager_(userManager),
  NetworkClient_(networkClient),
  PayoutProcessor_(payoutProcessor),
  FoundBlocksDb_(foundBlocksDb),
  PPLNSPayoutsDb_(pplnsPayoutsDb),
  PPSPayoutsDb_(ppsPayoutsDb),
  PPSHistoryDb_(ppsHistoryDb),
  State_(state),
  InstantPayoutTimer_(instantPayoutTimer)
{
}

const char *CAccountingApi::manualPayout(const std::string &user)
{
  auto It = State_.BalanceMap.find(user);
  if (It != State_.BalanceMap.end()) {
    auto &B = It->second;
    UInt<384> nonQueuedBalance = B.Balance - B.Requested;
    // Check global minimum (dust protection); per-user MinimalPayout is bypassed for manual payouts
    if (!nonQueuedBalance.isNegative() &&
        nonQueuedBalance >= State_.BackendSettings.load(std::memory_order_relaxed).PayoutConfig.InstantMinimalPayout) {
      auto batch = CAccountingState::batch();
      bool result = PayoutProcessor_.requestManualPayout(user, batch);
      if (result) {
        State_.addPayoutQueue(batch);
        State_.flushState(batch);
        CLOG_F(INFO, "Manual payout success for {}", user);
        return "ok";
      } else {
        return "payout_error";
      }
    } else {
      return "insufficient_balance";
    }
  } else {
    return "no_balance";
  }
}

void CAccountingApi::queryFoundBlocks(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback)
{
  std::unique_ptr<rocksdbBase::IteratorType> It(FoundBlocksDb_.iterator());
  if (heightFrom != -1) {
    FoundBlockRecord blk;
    blk.Height = heightFrom;
    blk.Hash = hashFrom;
    It->seek(blk);
    It->prev();
  } else {
    It->seekLast();
  }

  std::vector<CNetworkClient::GetBlockConfirmationsQuery> confirmationsQuery;
  std::vector<FoundBlockRecord> foundBlocks;
  for (unsigned i = 0; i < count && It->valid(); i++) {
    FoundBlockRecord dbBlock;
    RawData data = It->value();
    if (!dbBlock.deserializeValue(data.data, data.size))
      break;

    // Replace login with public name
    UserManager::Credentials credentials;
    if (UserManager_.getUserCredentials(dbBlock.FoundBy, credentials) && !credentials.Name.empty())
      dbBlock.FoundBy = credentials.Name;

    foundBlocks.push_back(dbBlock);
    confirmationsQuery.emplace_back(dbBlock.Hash, dbBlock.Height);
    It->prev();
  }

  // query confirmations
  if (count)
    NetworkClient_.ioGetBlockConfirmations(Base_, Cfg_.RequiredConfirmations, confirmationsQuery);

  callback(foundBlocks, confirmationsQuery);
}

std::vector<CPPLNSPayoutEntry> CAccountingApi::queryPPLNSPayouts(const std::string &login, int64_t timeFrom, const std::string &hashFrom, uint32_t count)
{
  std::unique_ptr<rocksdbBase::IteratorType> It(PPLNSPayoutsDb_.iterator());

  CPPLNSPayout valueRecord;
  xmstream resumeKey;
  auto validPredicate = [&login](const CPPLNSPayout &record) -> bool {
    return record.Login == login;
  };

  {
    CPPLNSPayout record;
    record.Login = login;
    record.RoundStartTime = Timestamp(std::numeric_limits<int64_t>::max());
    record.serializeKey(resumeKey);
  }

  {
    CPPLNSPayout keyRecord;
    keyRecord.Login = login;
    keyRecord.RoundStartTime = timeFrom == 0 ? Timestamp(std::numeric_limits<int64_t>::max()) : Timestamp::fromUnixTime(timeFrom);
    keyRecord.BlockHash = hashFrom;
    It->seekForPrev<CPPLNSPayout>(keyRecord, resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  unsigned fractionalPartSize = CoinInfo_.FractionalPartSize;
  double rateScale = std::pow(10.0, 8 - static_cast<int>(fractionalPartSize));

  std::vector<CPPLNSPayoutEntry> payouts;
  for (unsigned i = 0; i < count && It->valid(); i++) {
    CPPLNSPayoutEntry entry;
    entry.StartTime = valueRecord.RoundStartTime;
    entry.EndTime = valueRecord.RoundEndTime;
    entry.Hash = std::move(valueRecord.BlockHash);
    entry.Height = valueRecord.BlockHeight;
    entry.Value = valueRecord.PayoutValue;
    entry.ValueBTC = valueRecord.PayoutValue;
    entry.ValueBTC.mulfp(valueRecord.RateToBTC * rateScale);
    entry.ValueUSD = valueRecord.PayoutValue;
    entry.ValueUSD.mulfp(valueRecord.RateToBTC * valueRecord.RateBTCToUSD * rateScale);
    payouts.emplace_back(std::move(entry));
    It->prev<CPPLNSPayout>(resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  return payouts;
}

std::vector<CAccumulatedPayoutEntry> CAccountingApi::queryPPLNSPayoutsAcc(const std::string &login, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval)
{
  std::vector<CAccumulatedPayoutEntry> payoutAccs;

  // Maximum number of output cells to prevent excessive memory/CPU usage
  constexpr int64_t MaxOutputCells = 3200;

  // TODO: return error
  if (timeTo <= timeFrom ||
      groupByInterval <= 0 ||
      (timeTo - timeFrom) % groupByInterval != 0 ||
      (timeTo - timeFrom) / groupByInterval > MaxOutputCells) {
    return payoutAccs;
  }

  std::unique_ptr<rocksdbBase::IteratorType> It(PPLNSPayoutsDb_.iterator());

  CPPLNSPayout valueRecord;
  xmstream resumeKey;
  auto validPredicate = [&login](const CPPLNSPayout &record) -> bool {
    return record.Login == login;
  };

  Timestamp gridStart = Timestamp::fromUnixTime(timeFrom);
  Timestamp gridEnd = Timestamp::fromUnixTime(timeTo);
  auto groupBy = std::chrono::seconds(groupByInterval);
  size_t count = (gridEnd - gridStart) / groupBy;

  payoutAccs.resize(count);
  for (size_t i = 0; i < count; i++)
    payoutAccs[i].TimeLabel = timeFrom + groupByInterval * static_cast<int64_t>(i + 1);

  {
    CPPLNSPayout record;
    record.Login = login;
    record.RoundStartTime = Timestamp(std::numeric_limits<int64_t>::max());
    record.BlockHash.clear();
    record.serializeKey(resumeKey);
  }

  {
    CPPLNSPayout keyRecord;
    keyRecord.Login = login;
    keyRecord.RoundStartTime = gridEnd;
    keyRecord.BlockHash.clear();
    It->seekForPrev<CPPLNSPayout>(keyRecord, resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  // Rounds form a continuous chain (each RoundStartTime == previous RoundEndTime),
  // so RoundEndTime decreases monotonically — safe to break early.
  while (It->valid()) {
    if (valueRecord.RoundEndTime <= gridStart)
      break;

    Timestamp clampedBegin = std::max(valueRecord.RoundStartTime, gridStart);
    Timestamp clampedEnd = std::min(valueRecord.RoundEndTime, gridEnd);

    size_t firstIdx = static_cast<size_t>((clampedBegin - gridStart) / groupBy);
    size_t lastIdx = static_cast<size_t>((clampedEnd - gridStart + groupBy - std::chrono::milliseconds(1)) / groupBy);

    Timestamp cellBegin = gridStart + groupBy * static_cast<int64_t>(firstIdx);
    Timestamp cellEnd = cellBegin + groupBy;
    for (size_t i = firstIdx; i < lastIdx; i++) {
      double coeff = overlapFraction(valueRecord.RoundStartTime, valueRecord.RoundEndTime, cellBegin, cellEnd);
      if (coeff >= 1.0)
        payoutAccs[i].merge(valueRecord.PayoutValue, valueRecord.RateToBTC, valueRecord.RateBTCToUSD, CoinInfo_.FractionalPartSize);
      else
        payoutAccs[i].mergeScaled(valueRecord.PayoutValue, valueRecord.RateToBTC, valueRecord.RateBTCToUSD, coeff, CoinInfo_.FractionalPartSize);

      cellBegin = cellEnd;
      cellEnd += groupBy;
    }

    It->prev<CPPLNSPayout>(resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  return payoutAccs;
}

std::vector<CAccumulatedPayoutEntry> CAccountingApi::queryPPSPayoutsAcc(
  const std::string &login,
  int64_t timeFrom,
  int64_t timeTo,
  int64_t groupByInterval)
{
  std::vector<CAccumulatedPayoutEntry> payoutAccs;

  constexpr int64_t MaxOutputCells = 3200;
  if (timeTo <= timeFrom ||
      groupByInterval <= 0 ||
      (timeTo - timeFrom) % groupByInterval != 0 ||
      (timeTo - timeFrom) / groupByInterval > MaxOutputCells) {
    return payoutAccs;
  }

  std::unique_ptr<rocksdbBase::IteratorType> It(PPSPayoutsDb_.iterator());

  CPPSPayout valueRecord;
  xmstream resumeKey;
  auto validPredicate = [&login](const CPPSPayout &record) -> bool {
    return record.Login == login;
  };

  Timestamp gridStart = Timestamp::fromUnixTime(timeFrom);
  Timestamp gridEnd = Timestamp::fromUnixTime(timeTo);
  auto groupBy = std::chrono::seconds(groupByInterval);
  size_t count = (gridEnd - gridStart) / groupBy;

  payoutAccs.resize(count);
  for (size_t i = 0; i < count; i++)
    payoutAccs[i].TimeLabel = timeFrom + groupByInterval * static_cast<int64_t>(i + 1);

  {
    CPPSPayout record;
    record.Login = login;
    record.IntervalBegin = Timestamp(std::numeric_limits<int64_t>::max());
    record.serializeKey(resumeKey);
  }

  {
    CPPSPayout keyRecord;
    keyRecord.Login = login;
    keyRecord.IntervalBegin = gridEnd;
    It->seekForPrev<CPPSPayout>(keyRecord, resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  while (It->valid()) {
    if (valueRecord.IntervalEnd <= gridStart)
      break;

    Timestamp clampedBegin = std::max(valueRecord.IntervalBegin, gridStart);
    Timestamp clampedEnd = std::min(valueRecord.IntervalEnd, gridEnd);

    size_t firstIdx = static_cast<size_t>((clampedBegin - gridStart) / groupBy);
    size_t lastIdx = static_cast<size_t>((clampedEnd - gridStart + groupBy - std::chrono::milliseconds(1)) / groupBy);

    Timestamp cellBegin = gridStart + groupBy * static_cast<int64_t>(firstIdx);
    Timestamp cellEnd = cellBegin + groupBy;
    for (size_t i = firstIdx; i < lastIdx; i++) {
      double coeff = overlapFraction(valueRecord.IntervalBegin, valueRecord.IntervalEnd, cellBegin, cellEnd);
      if (coeff >= 1.0)
        payoutAccs[i].merge(valueRecord.PayoutValue, valueRecord.RateToBTC, valueRecord.RateBTCToUSD, CoinInfo_.FractionalPartSize);
      else
        payoutAccs[i].mergeScaled(valueRecord.PayoutValue, valueRecord.RateToBTC, valueRecord.RateBTCToUSD, coeff, CoinInfo_.FractionalPartSize);

      cellBegin = cellEnd;
      cellEnd += groupBy;
    }

    It->prev<CPPSPayout>(resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  return payoutAccs;
}

std::vector<CPPSPayoutEntry> CAccountingApi::queryPPSPayouts(const std::string &login, int64_t timeFrom, uint32_t count)
{
  std::unique_ptr<rocksdbBase::IteratorType> It(PPSPayoutsDb_.iterator());

  CPPSPayout valueRecord;
  xmstream resumeKey;
  auto validPredicate = [&login](const CPPSPayout &record) -> bool {
    return record.Login == login;
  };

  {
    CPPSPayout record;
    record.Login = login;
    record.IntervalBegin = Timestamp(std::numeric_limits<int64_t>::max());
    record.serializeKey(resumeKey);
  }

  {
    CPPSPayout keyRecord;
    keyRecord.Login = login;
    keyRecord.IntervalBegin = timeFrom == 0 ? Timestamp(std::numeric_limits<int64_t>::max()) : Timestamp::fromUnixTime(timeFrom);
    It->seekForPrev<CPPSPayout>(keyRecord, resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  unsigned fractionalPartSize = CoinInfo_.FractionalPartSize;
  double rateScale = std::pow(10.0, 8 - static_cast<int>(fractionalPartSize));

  std::vector<CPPSPayoutEntry> payouts;
  for (unsigned i = 0; i < count && It->valid(); i++) {
    CPPSPayoutEntry entry;
    entry.StartTime = valueRecord.IntervalBegin;
    entry.EndTime = valueRecord.IntervalEnd;
    entry.Value = valueRecord.PayoutValue;
    entry.ValueBTC = valueRecord.PayoutValue;
    entry.ValueBTC.mulfp(valueRecord.RateToBTC * rateScale);
    entry.ValueUSD = valueRecord.PayoutValue;
    entry.ValueUSD.mulfp(valueRecord.RateToBTC * valueRecord.RateBTCToUSD * rateScale);
    payouts.emplace_back(std::move(entry));
    It->prev<CPPSPayout>(resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  return payouts;
}

std::vector<double> CAccountingApi::poolLuck(const std::vector<int64_t> &intervals)
{
  if (!std::is_sorted(intervals.begin(), intervals.end()))
    return {};

  Timestamp currentTime = Timestamp::now();
  std::vector<double> result;

  std::unique_ptr<rocksdbBase::IteratorType> It(FoundBlocksDb_.iterator());
  It->seekLast();

  auto intervalIt = intervals.begin();
  if (intervalIt == intervals.end())
    return result;

  UInt<256> acceptedWork = UInt<256>::zero();
  UInt<256> expectedWork = UInt<256>::zero();
  for (const auto &score: State_.CurrentScores)
    acceptedWork += score.second;

  Timestamp currentTimePoint = currentTime - std::chrono::seconds(*intervalIt);
  while (It->valid()) {
    FoundBlockRecord dbBlock;
    RawData data = It->value();
    if (!dbBlock.deserializeValue(data.data, data.size))
      break;

    while (dbBlock.Time < currentTimePoint) {
      result.push_back(expectedWork.nonZero() ? UInt<256>::fpdiv(acceptedWork, expectedWork) : 0.0);
      if (++intervalIt == intervals.end())
        return result;

      currentTimePoint = currentTime - std::chrono::seconds(*intervalIt);
    }

    if (dbBlock.ExpectedWork.nonZero()) {
      acceptedWork += dbBlock.AccumulatedWork;
      expectedWork += dbBlock.ExpectedWork;
    }

    It->prev();
  }

  while (intervalIt++ != intervals.end())
    result.push_back(expectedWork.nonZero() ? UInt<256>::fpdiv(acceptedWork, expectedWork) : 0.0);
  return result;
}

CPPSState CAccountingApi::queryPPSState()
{
  return State_.PPSState;
}

std::pair<CRoundBestShareData, double> CAccountingApi::queryRoundInfo()
{
  UInt<256> acceptedWork = UInt<256>::zero();
  for (const auto &score : State_.CurrentScores)
    acceptedWork += score.second;
  double acceptedDifficulty = State_.RoundBestShare.ExpectedWork.nonZero()
    ? UInt<256>::fpdiv(acceptedWork, State_.RoundBestShare.ExpectedWork)
    : 0.0;
  return {State_.RoundBestShare, acceptedDifficulty};
}

std::vector<CPPSState> CAccountingApi::queryPPSHistory(int64_t timeFrom, int64_t timeTo)
{
  std::unique_ptr<rocksdbBase::IteratorType> It(PPSHistoryDb_.iterator());

  CPPSState searchKey;
  searchKey.Time = Timestamp::fromUnixTime(timeFrom);
  It->seek(searchKey);

  Timestamp limit = Timestamp::fromUnixTime(timeTo);
  std::vector<CPPSState> result;
  while (It->valid()) {
    CPPSState record;
    RawData data = It->value();
    if (!record.deserializeValue(data.data, data.size))
      break;
    if (record.Time > limit)
      break;
    result.emplace_back(record);
    It->next();
  }

  return result;
}

const char *CAccountingApi::updateBackendSettings(const std::optional<CBackendPPS> &pps,
                                                  const std::optional<CBackendPayouts> &payouts,
                                                  const std::optional<CBackendSwap> &swap)
{
  CBackendSettings settings = State_.BackendSettings.load(std::memory_order_relaxed);

  if (pps.has_value()) {
    if (pps->PoolFee < 0.0 || pps->PoolFee > 100.0)
      return "invalid_pool_fee";

    if (pps->SaturationFunction != ESaturationFunction::None) {
      if (pps->SaturationB0 <= 0.0 ||
          pps->SaturationANegative < 0.0 || pps->SaturationANegative > 1.0 ||
          pps->SaturationAPositive < 0.0 || pps->SaturationAPositive > 1.0) {
        return "invalid_saturation_params";
      }
    }

    settings.PPSConfig = *pps;
    CLOG_F(INFO,
      "[{}] PPS config updated: enabled={}, poolFee={:.2f}, saturation={}, B0={:.4f}, aNeg={:.4f}, aPos={:.4f}",
      CoinInfo_.Name,
      static_cast<int>(pps->Enabled),
      pps->PoolFee,
      ESaturationFunctionToString(pps->SaturationFunction),
      pps->SaturationB0,
      pps->SaturationANegative,
      pps->SaturationAPositive);
  }

  if (payouts.has_value()) {
    if (payouts->InstantPayoutInterval <= std::chrono::minutes(0) ||
        payouts->RegularPayoutInterval <= std::chrono::hours(0) ||
        payouts->RegularPayoutDayOffset < std::chrono::hours(0)) {
      return "invalid_payout_interval";
    }

    {
      int64_t hours = payouts->RegularPayoutInterval.count();
      if (hours < 24 ? (24 % hours != 0) : (hours % 24 != 0))
        return "invalid_regular_payout_interval";
      if (payouts->RegularPayoutDayOffset >= std::chrono::hours(24))
        return "invalid_regular_payout_day_offset";
    }

    settings.PayoutConfig = *payouts;
    CLOG_F(INFO, "[{}] Payouts config updated", CoinInfo_.Name);
  }

  if (swap.has_value()) {
    settings.SwapConfig = *swap;
    CLOG_F(INFO,
      "[{}] Swap config updated: acceptIncoming={}, acceptOutgoing={}",
      CoinInfo_.Name,
      static_cast<int>(swap->AcceptIncoming),
      static_cast<int>(swap->AcceptOutgoing));
  }

  State_.BackendSettings.store(settings, std::memory_order_relaxed);
  State_.flushBackendSettings();
  if (settings.PayoutConfig.InstantPayoutsEnabled) {
    InstantPayoutTimer_.setInterval(std::chrono::duration_cast<std::chrono::microseconds>(settings.PayoutConfig.InstantPayoutInterval));
    InstantPayoutTimer_.resume();
  } else {
    InstantPayoutTimer_.pause();
  }
  return "ok";
}

UserBalanceInfo CAccountingApi::queryBalance(const std::string &user)
{
  UserBalanceInfo info;

  // Calculate queued balance
  info.Queued = UInt<384>::zero();
  for (const auto &round: State_.ActiveRounds) {
    auto payout = std::lower_bound(round.Payouts.begin(), round.Payouts.end(), user, [](const CUserPayout &record, const std::string &user) -> bool { return record.UserId < user; });
    if (payout != round.Payouts.end() && payout->UserId == user)
      info.Queued += payout->Value;
  }
  auto It = State_.BalanceMap.find(user);
  if (It != State_.BalanceMap.end()) {
    info.Data = It->second;
  } else {
    info.Data.Login = user;
    info.Data.Balance = UInt<384>::zero();
    info.Data.Requested = UInt<384>::zero();
    info.Data.Paid = UInt<384>::zero();
  }

  return info;
}
