#include "poolcore/accountingState.h"
#include "loguru.hpp"
#include <cinttypes>

CAccountingState::CAccountingState(const std::filesystem::path &dbPath)
  : DbPath_(dbPath),
    Db(dbPath / "accounting.state")
{
}

bool CAccountingState::load(const CCoinInfo &coinInfo)
{
  RecentStats.clear();
  CurrentScores.clear();

  bool settingsLoaded = false;
  std::unique_ptr<rocksdbBase::IteratorType> It(Db.iterator());
  It->seekFirst();

  while (It->valid()) {
    RawData key = It->key();
    RawData value = It->value();
    std::string keyStr(reinterpret_cast<const char*>(key.data), key.size);

    xmstream stream(value.data, value.size);
    stream.seekSet(0);

    if (keyStr == "lastmsgid") {
      DbIo<uint64_t>::unserialize(stream, SavedShareId);
    } else if (keyStr == "recentstats") {
      DbIo<decltype(RecentStats)>::unserialize(stream, RecentStats);
    } else if (keyStr == "currentscores") {
      DbIo<decltype(CurrentScores)>::unserialize(stream, CurrentScores);
    } else if (keyStr == "ppspending") {
      DbIo<decltype(PPSPendingBalance)>::unserialize(stream, PPSPendingBalance);
    } else if (keyStr == "currentroundstart") {
      DbIo<Timestamp>::unserialize(stream, CurrentRoundStartTime);
    } else if (keyStr == "settings") {
      CBackendSettings settings;
      DbIo<CBackendSettings>::unserialize(stream, settings);
      BackendSettings.store(settings, std::memory_order_relaxed);
      settingsLoaded = true;
    } else if (keyStr == "ppsstate") {
      DbIo<CPPSState>::unserialize(stream, PPSState);
    } else if (keyStr == "payoutqueue") {
      while (stream.remaining()) {
        PayoutDbRecord element;
        if (!element.deserializeValue(stream))
          break;
        PayoutQueue.push_back(element);
        KnownTransactions.insert(element.TransactionId);
      }
    } else if (!keyStr.empty() && keyStr[0] == 'r') {
      MiningRound round;
      if (round.deserializeValue(stream))
        ActiveRounds.push_back(std::move(round));
      else
        LOG_F(ERROR, "Failed to deserialize active round");
    }

    It->next();
  }

  // Fresh DB has no currentroundstart key; use current time as round start
  if (CurrentRoundStartTime == Timestamp())
    CurrentRoundStartTime = Timestamp::now();

  // Fresh DB has no settings key; initialize payout thresholds from coin library defaults
  if (!settingsLoaded) {
    CBackendSettings settings;
    settings.PayoutConfig.InstantMinimalPayout = coinInfo.DefaultInstantMinimalPayout;
    settings.PayoutConfig.RegularMinimalPayout = coinInfo.DefaultRegularMinimalPayout;
    BackendSettings.store(settings, std::memory_order_relaxed);
  }

  LastAcceptedMsgId_ = SavedShareId;

  if (SavedShareId != 0) {
    LOG_F(INFO, "AccountingDb: loaded state from db, SavedShareId=%" PRIu64 "", SavedShareId);
    return true;
  }
  return false;
}

void CAccountingState::addMutableState(rocksdbBase::CBatch &batch)
{
  SavedShareId = LastAcceptedMsgId_;

  {
    xmstream stream;
    DbIo<uint64_t>::serialize(stream, LastAcceptedMsgId_);
    std::string key = "lastmsgid";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }

  {
    std::string key = "currentscores";
    if (CurrentScores.empty()) {
      batch.deleteRow(key.data(), key.size());
    } else {
      xmstream stream;
      DbIo<decltype(CurrentScores)>::serialize(stream, CurrentScores);
      batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
    }
  }

  {
    std::string key = "ppspending";
    if (PPSPendingBalance.empty()) {
      batch.deleteRow(key.data(), key.size());
    } else {
      xmstream stream;
      DbIo<decltype(PPSPendingBalance)>::serialize(stream, PPSPendingBalance);
      batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
    }
  }

  {
    xmstream stream;
    DbIo<CPPSState>::serialize(stream, PPSState);
    std::string key = "ppsstate";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }
}

void CAccountingState::addRoundState(rocksdbBase::CBatch &batch)
{
  {
    xmstream stream;
    DbIo<decltype(RecentStats)>::serialize(stream, RecentStats);
    std::string key = "recentstats";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }

  {
    xmstream stream;
    DbIo<Timestamp>::serialize(stream, CurrentRoundStartTime);
    std::string key = "currentroundstart";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }
}

void CAccountingState::flushState(rocksdbBase::CBatch &batch)
{
  Db.writeBatch(batch);
}

void CAccountingState::flushBackendSettings()
{
  xmstream stream;
  DbIo<CBackendSettings>::serialize(stream, BackendSettings.load(std::memory_order_relaxed));
  Db.put("default", "settings", 8, stream.data(), stream.sizeOf());
}

void CAccountingState::addPayoutQueue(rocksdbBase::CBatch &batch)
{
  xmstream stream;
  for (auto &p: PayoutQueue)
    p.serializeValue(stream);

  std::string key = "payoutqueue";
  batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
}

void CAccountingState::applyBatch(uint64_t msgId, const CAccountingStateBatch &batch)
{
  if (msgId <= LastAcceptedMsgId_)
    return;
  LastAcceptedMsgId_ = msgId;
  PPSState.LastSaturateCoeff = batch.LastSaturateCoeff;
  PPSState.LastBaseBlockReward = batch.LastBaseBlockReward;
  PPSState.LastAverageTxFee = batch.LastAverageTxFee;
  for (const auto &[user, work] : batch.PPLNSScores)
    CurrentScores[user] += work;
  for (const auto &[user, amount] : batch.PPSBalances) {
    PPSPendingBalance[user] += amount;
    PPSState.Balance -= amount;
  }
  if (batch.PPSReferenceCost.nonZero()) {
    PPSState.ReferenceBalance -= batch.PPSReferenceCost;
    PPSState.updateMinMax(Timestamp::now());
  }
}
