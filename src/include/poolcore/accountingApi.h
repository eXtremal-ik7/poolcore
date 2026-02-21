#pragma once

#include "accountingData.h"
#include "accountingPayouts.h"
#include "clientDispatcher.h"
#include "usermgr.h"
#include <set>
#include <optional>

class CAccountingState;

struct UserBalanceInfo {
  UserBalanceRecord Data;
  UInt<384> Queued;
};

struct CPPLNSPayoutInfo {
  Timestamp RoundStartTime;
  Timestamp RoundEndTime;
  std::string BlockHash;
  uint64_t BlockHeight;
  UInt<384> Value;
  UInt<384> ValueBTC;
  UInt<384> ValueUSD;
};

struct CPPSPayoutInfo {
  Timestamp IntervalBegin;
  Timestamp IntervalEnd;
  UInt<384> Value;
  UInt<384> ValueBTC;
  UInt<384> ValueUSD;
};

struct CPPLNSPayoutAcc {
  int64_t IntervalEnd = 0;
  UInt<384> TotalCoin = UInt<384>::zero();
  UInt<384> TotalBTC = UInt<384>::zero();
  UInt<384> TotalUSD = UInt<384>::zero();

  void merge(const CPPLNSPayout &record, unsigned fractionalPartSize);
  void mergeScaled(const CPPLNSPayout &record, double coeff, unsigned fractionalPartSize);
};

struct CPPSPayoutAcc {
  int64_t IntervalEnd = 0;
  UInt<384> TotalCoin = UInt<384>::zero();
  UInt<384> TotalBTC = UInt<384>::zero();
  UInt<384> TotalUSD = UInt<384>::zero();

  void merge(const CPPSPayout &record, unsigned fractionalPartSize);
  void mergeScaled(const CPPSPayout &record, double coeff, unsigned fractionalPartSize);
};

using QueryFoundBlocksCallback = std::function<void(const std::vector<FoundBlockRecord>&, const std::vector<CNetworkClient::GetBlockConfirmationsQuery>&)>;

class CAccountingApi {
public:
  CAccountingApi(asyncBase *base,
                 const PoolBackendConfig &cfg,
                 const CCoinInfo &coinInfo,
                 UserManager &userManager,
                 CNetworkClientDispatcher &clientDispatcher,
                 CPayoutProcessor &payoutProcessor,
                 CAccountingState &state,
                 kvdb<rocksdbBase> &foundBlocksDb,
                 kvdb<rocksdbBase> &pplnsPayoutsDb,
                 kvdb<rocksdbBase> &ppsPayoutsDb,
                 kvdb<rocksdbBase> &ppsHistoryDb,
                 std::map<std::string, UserBalanceRecord> &balanceMap,
                 const std::set<MiningRound*> &unpayedRounds);

  const char *manualPayout(const std::string &user);
  void queryFoundBlocks(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback);
  std::vector<CPPLNSPayoutInfo> queryPPLNSPayouts(const std::string &login, int64_t timeFrom, const std::string &hashFrom, uint32_t count);
  std::vector<CPPLNSPayoutAcc> queryPPLNSPayoutsAcc(const std::string &login, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval);
  std::vector<CPPSPayoutInfo> queryPPSPayouts(const std::string &login, int64_t timeFrom, uint32_t count);
  std::vector<CPPSPayoutAcc> queryPPSPayoutsAcc(const std::string &login, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval);
  std::vector<CPPSState> queryPPSHistory(int64_t timeFrom, int64_t timeTo);
  UserBalanceInfo queryBalance(const std::string &user);
  std::vector<double> poolLuck(const std::vector<int64_t> &intervals);
  CPPSState queryPPSState();
  const char *updateBackendSettings(const std::optional<CBackendSettings::PPS> &pps,
                                    const std::optional<CBackendSettings::Payouts> &payouts);

  void setInstantPayoutEvent(aioUserEvent *event) { InstantPayoutEvent_ = event; }

private:
  asyncBase *Base_;
  const PoolBackendConfig &Cfg_;
  const CCoinInfo &CoinInfo_;
  UserManager &UserManager_;
  CNetworkClientDispatcher &ClientDispatcher_;
  CPayoutProcessor &PayoutProcessor_;
  kvdb<rocksdbBase> &FoundBlocksDb_;
  kvdb<rocksdbBase> &PPLNSPayoutsDb_;
  kvdb<rocksdbBase> &PPSPayoutsDb_;
  kvdb<rocksdbBase> &PPSHistoryDb_;
  std::map<std::string, UserBalanceRecord> &BalanceMap_;
  const std::set<MiningRound*> &UnpayedRounds_;
  CAccountingState &State_;
  aioUserEvent *InstantPayoutEvent_ = nullptr;
};
