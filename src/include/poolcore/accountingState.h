#pragma once

#include "accountingData.h"
#include "backendData.h"
#include "poolcore/rocksdbBase.h"
#include "statsData.h"
#include <atomic>
#include <filesystem>
#include <list>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CAccountingState {
public:
  // Persistent state (serialized to accounting.state RocksDB)
  std::map<std::string, UInt<256>> CurrentScores;             // key: ".currentscores"
  std::unordered_map<std::string, UInt<384>> PPSPendingBalance; // key: ".ppspending"
  std::vector<CStatsExportData> RecentStats;                  // key: ".recentstats"
  uint64_t SavedShareId = 0;                                  // key: ".lastmsgid"
  Timestamp CurrentRoundStartTime;                            // key: ".currentroundstart"
  std::list<PayoutDbRecord> PayoutQueue;                      // key: ".payoutqueue"
  std::atomic<CBackendSettings> BackendSettings;              // key: ".settings"
  CPPSState PPSState;                                         // key: ".ppsstate"
  std::list<MiningRound> ActiveRounds;                        // keys: 'r' + Height + BlockHash
  std::map<std::string, UserBalanceRecord> BalanceMap;        // keys: 'b' + Login

public:
  // In-memory state (not serialized; rebuilt from PayoutQueue on load)
  std::unordered_set<std::string> KnownTransactions;

public:
  CAccountingState(const std::filesystem::path &dbPath);
  bool load(const CCoinInfo &coinInfo);
  uint64_t lastAcceptedMsgId() const { return LastAcceptedMsgId_; }

  void applyBatch(uint64_t msgId, const CAccountingStateBatch &batch);
  static rocksdbBase::CBatch batch() { return rocksdbBase::CBatch{"default", rocksdb::WriteBatch()}; }
  void addMutableState(rocksdbBase::CBatch &batch);
  void addRoundState(rocksdbBase::CBatch &batch);
  void addPayoutQueue(rocksdbBase::CBatch &batch);
  void putBalance(rocksdbBase::CBatch &batch, const UserBalanceRecord &balance);
  void putRound(rocksdbBase::CBatch &batch, const MiningRound &round);
  void deleteRound(rocksdbBase::CBatch &batch, const MiningRound &round);
  void flushState(rocksdbBase::CBatch &batch);
  void flushBackendSettings();
  uint64_t stateId() const { return StateId_; }

private:
  uint64_t LastAcceptedMsgId_ = 0;
  uint64_t StateId_ = 0;                                     // key: ".stateid"
  std::filesystem::path DbPath_;
  rocksdbBase Db;
};
