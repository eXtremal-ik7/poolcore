#pragma once

#include "accountingData.h"
#include "clientDispatcher.h"
#include "usermgr.h"
#include <list>
#include <set>

class CPayoutProcessor {
public:
  CPayoutProcessor(asyncBase *base,
                   const PoolBackendConfig &cfg,
                   const CCoinInfo &coinInfo,
                   CNetworkClientDispatcher &clientDispatcher,
                   kvdb<rocksdbBase> &payoutDb,
                   kvdb<rocksdbBase> &balanceDb,
                   kvdb<rocksdbBase> &poolBalanceDb,
                   std::list<PayoutDbRecord> &payoutQueue,
                   std::unordered_set<std::string> &knownTransactions,
                   std::map<std::string, UserBalanceRecord> &balanceMap,
                   const std::unordered_map<std::string, UserSettingsRecord> &userSettings,
                   const std::atomic<CBackendSettings> &backendSettings);

  void makePayout();
  bool requestManualPayout(const std::string &address);
  void checkBalance(const std::set<MiningRound*> &unpayedRounds, const CPPSState &ppsState);

private:
  void buildTransaction(PayoutDbRecord &payout, unsigned index, std::string &recipient, bool *needSkipPayout);
  bool sendTransaction(PayoutDbRecord &payout);
  bool checkTxConfirmations(PayoutDbRecord &payout);

private:
  asyncBase *Base_;
  const PoolBackendConfig &Cfg_;
  const CCoinInfo &CoinInfo_;
  CNetworkClientDispatcher &ClientDispatcher_;
  kvdb<rocksdbBase> &PayoutDb_;
  kvdb<rocksdbBase> &BalanceDb_;
  kvdb<rocksdbBase> &PoolBalanceDb_;

  std::list<PayoutDbRecord> &PayoutQueue_;
  std::unordered_set<std::string> &KnownTransactions_;
  std::map<std::string, UserBalanceRecord> &BalanceMap_;
  const std::unordered_map<std::string, UserSettingsRecord> &UserSettings_;
  const std::atomic<CBackendSettings> &BackendSettings_;
};
