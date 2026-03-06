#pragma once

#include "accountingData.h"
#include "accountingPayouts.h"
#include "clientDispatcher.h"
#include "usermgr.h"
#include "poolcommon/periodicTimer.h"
#include <set>
#include <optional>

class CAccountingState;

struct UserBalanceInfo {
  UserBalanceRecord Data;
  UInt<384> Queued;
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
                 CPeriodicTimer &instantPayoutTimer,
                 kvdb<rocksdbBase> &foundBlocksDb,
                 kvdb<rocksdbBase> &pplnsPayoutsDb,
                 kvdb<rocksdbBase> &ppsPayoutsDb,
                 kvdb<rocksdbBase> &ppsHistoryDb);

  const char *manualPayout(const std::string &user);
  void queryFoundBlocks(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback);
  std::vector<CPPLNSPayoutEntry> queryPPLNSPayouts(const std::string &login, int64_t timeFrom, const std::string &hashFrom, uint32_t count);
  std::vector<CAccumulatedPayoutEntry> queryPPLNSPayoutsAcc(const std::string &login, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval);
  std::vector<CPPSPayoutEntry> queryPPSPayouts(const std::string &login, int64_t timeFrom, uint32_t count);
  std::vector<CAccumulatedPayoutEntry> queryPPSPayoutsAcc(const std::string &login, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval);
  std::vector<CPPSState> queryPPSHistory(int64_t timeFrom, int64_t timeTo);
  UserBalanceInfo queryBalance(const std::string &user);
  std::vector<double> poolLuck(const std::vector<int64_t> &intervals);
  CPPSState queryPPSState();
  const char *updateBackendSettings(const std::optional<CBackendPPS> &pps,
                                    const std::optional<CBackendPayouts> &payouts,
                                    const std::optional<CBackendSwap> &swap);

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
  CAccountingState &State_;
  CPeriodicTimer &InstantPayoutTimer_;
};
