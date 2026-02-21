#pragma once

#include "accountingData.h"
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
  std::map<std::string, UInt<256>> CurrentScores;             // key: "currentscores"
  std::unordered_map<std::string, UInt<384>> PPSPendingBalance; // key: "ppspending"
  std::vector<CStatsExportData> RecentStats;                  // key: "recentstats"
  uint64_t SavedShareId = 0;                                  // key: "lastmsgid"
  Timestamp CurrentRoundStartTime;                            // key: "currentroundstart"
  std::list<PayoutDbRecord> PayoutQueue;                      // key: "payoutqueue"
  std::atomic<CBackendSettings> BackendSettings;              // key: "settings"
  CPPSState PPSState;                                         // key: "ppsstate"

public:
  // In-memory state (not serialized; rebuilt from PayoutQueue on load)
  std::unordered_set<std::string> KnownTransactions;

public:
  CAccountingState(const std::filesystem::path &dbPath);
  bool load(const CCoinInfo &coinInfo);
  uint64_t lastAcceptedMsgId() const { return LastAcceptedMsgId_; }

  void applyBatch(uint64_t msgId, const CAccountingStateBatch &batch);
  void flushState();
  void flushBackendSettings();
  void flushPayoutQueue();

private:
  uint64_t LastAcceptedMsgId_ = 0;
  std::filesystem::path DbPath_;
  rocksdbBase Db;
};
