#pragma once

#include "accountingData.h"
#include "clientDispatcher.h"
#include "usermgr.h"
#include <list>
#include <set>

class CAccountingState;

class CPayoutProcessor {
public:
  CPayoutProcessor(asyncBase *base,
                   const PoolBackendConfig &cfg,
                   const CCoinInfo &coinInfo,
                   CNetworkClientDispatcher &clientDispatcher,
                   CAccountingState &state,
                   const std::set<MiningRound*> &unpayedRounds,
                   kvdb<rocksdbBase> &payoutDb,
                   kvdb<rocksdbBase> &balanceDb,
                   kvdb<rocksdbBase> &poolBalanceDb,
                   std::map<std::string, UserBalanceRecord> &balanceMap,
                   const std::unordered_map<std::string, UserSettingsRecord> &userSettings);

  void makePayout();
  bool requestManualPayout(const std::string &address);
  void checkBalance();

private:
  void buildTransaction(PayoutDbRecord &payout, unsigned index, std::string &recipient, bool *needSkipPayout);
  bool sendTransaction(PayoutDbRecord &payout);
  bool checkTxConfirmations(PayoutDbRecord &payout);

private:
  asyncBase *Base_;
  const PoolBackendConfig &Cfg_;
  const CCoinInfo &CoinInfo_;
  CNetworkClientDispatcher &ClientDispatcher_;
  CAccountingState &State_;
  const std::set<MiningRound*> &UnpayedRounds_;
  kvdb<rocksdbBase> &PayoutDb_;
  kvdb<rocksdbBase> &BalanceDb_;
  kvdb<rocksdbBase> &PoolBalanceDb_;
  std::map<std::string, UserBalanceRecord> &BalanceMap_;
  const std::unordered_map<std::string, UserSettingsRecord> &UserSettings_;
};
