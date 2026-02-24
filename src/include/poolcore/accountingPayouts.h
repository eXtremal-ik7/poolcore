#pragma once

#include "accountingData.h"
#include "clientDispatcher.h"
#include "priceFetcher.h"
#include "usermgr.h"
#include <list>

class CAccountingState;

class CPayoutProcessor {
public:
  CPayoutProcessor(asyncBase *base,
                   const PoolBackendConfig &cfg,
                   const CCoinInfo &coinInfo,
                   CNetworkClientDispatcher &clientDispatcher,
                   CAccountingState &state,
                   kvdb<rocksdbBase> &payoutDb,
                   kvdb<rocksdbBase> &poolBalanceDb,
                   const std::unordered_map<std::string, UserSettingsRecord> &userSettings,
                   const CPriceFetcher &priceFetcher);

  void makePayout();
  bool requestManualPayout(const std::string &address, rocksdbBase::CBatch &batch);
  void checkBalance();

private:
  void buildTransaction(PayoutDbRecord &payout, unsigned index, std::string &recipient, bool *needSkipPayout, rocksdbBase::CBatch &batch);
  bool sendTransaction(PayoutDbRecord &payout);
  bool checkTxConfirmations(PayoutDbRecord &payout, rocksdbBase::CBatch &batch);

private:
  asyncBase *Base_;
  const PoolBackendConfig &Cfg_;
  const CCoinInfo &CoinInfo_;
  CNetworkClientDispatcher &ClientDispatcher_;
  CAccountingState &State_;
  kvdb<rocksdbBase> &PayoutDb_;
  kvdb<rocksdbBase> &PoolBalanceDb_;
  const std::unordered_map<std::string, UserSettingsRecord> &UserSettings_;
  const CPriceFetcher &PriceFetcher_;
};
