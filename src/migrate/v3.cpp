#include "migratecommon.h"
#include "struct2.h"
#include "poolcore/backendData.h"
#include "poolcore/coinLibrary.h"
#include "poolcore/poolCore.h"
#include "poolcore/accounting.h"
#include "poolcore/rocksdbBase.h"
#include "poolcore/shareLog.h"
#include "poolcommon/datFile.h"
#include "poolcommon/file.h"
#include "poolcommon/utils.h"
#include "poolcore/plugin.h"
#include "loguru.hpp"
#include <unordered_map>

// Legacy struct for migration from old .dat file format
struct CAccountingFileData {
  enum { CurrentRecordVersion = 1 };

  uint64_t LastShareId;
  int64_t LastBlockTime;
  std::vector<CStatsExportData> Recent;
  std::map<std::string, UInt<256>> CurrentScores;
};

template<>
struct DbIo<CAccountingFileData> {
  static inline void unserialize(xmstream &in, CAccountingFileData &data) {
    uint32_t version;
    DbIo<uint32_t>::unserialize(in, version);
    if (version == 1) {
      DbIo<decltype(data.LastShareId)>::unserialize(in, data.LastShareId);
      DbIo<decltype(data.LastBlockTime)>::unserialize(in, data.LastBlockTime);
      DbIo<decltype(data.Recent)>::unserialize(in, data.Recent);
      DbIo<decltype(data.CurrentScores)>::unserialize(in, data.CurrentScores);
    } else {
      in.seekEnd(0, true);
    }
  }
};

extern CPluginContext gPluginContext;

// Unconditional correction for rounding errors after conversion.
// Old data integrity is verified separately (strict checks on raw old values).
// Here we only distribute the rounding remainder evenly across all elements.
template<typename ValueType, typename Container, typename FieldAccessor>
static bool correctSum(Container &items, const ValueType &expected, uint64_t height, const char *fieldName, FieldAccessor accessor)
{
  ValueType sum = ValueType::zero();
  for (auto &item : items)
    sum += accessor(item);

  if (sum == expected)
    return true;

  bool needSubtract = sum > expected;
  ValueType diff = needSubtract ? sum - expected : expected - sum;

  ValueType div;
  uint64_t mod = diff.divmod64(items.size(), &div);
  uint64_t i = 0;
  for (auto &item : items) {
    if (needSubtract) {
      accessor(item) -= div;
      if (i < mod) accessor(item) -= 1u;
    } else {
      accessor(item) += div;
      if (i < mod) accessor(item) += 1u;
    }
    i++;
  }

  // Re-verify
  sum = ValueType::zero();
  for (auto &item : items)
    sum += accessor(item);
  if (sum != expected) {
    LOG_F(ERROR, "Round height=%llu: %s correction failed: sum=%s expected=%s",
          static_cast<unsigned long long>(height), fieldName, sum.getDecimal().c_str(), expected.getDecimal().c_str());
    return false;
  }

  LOG_F(WARNING, "Round height=%llu: adjusted %s diff=%s across %zu items",
        static_cast<unsigned long long>(height), fieldName, diff.getDecimal().c_str(), items.size());
  return true;
}

static bool isNonCoinDirectory(const std::string &name)
{
  static const char *skip[] = {"useractions", "userfeeplan", "users", "usersessions", "usersettings"};
  for (const char *prefix : skip) {
    if (name.starts_with(prefix))
      return true;
  }
  return false;
}

static bool loadCoinMap(const std::filesystem::path &srcPath,
                        std::unordered_map<std::string, CCoinInfo> &coinMap,
                        std::unordered_map<std::string, CCoinInfoOld2> &old2Map)
{
  for (std::filesystem::directory_iterator I(srcPath), IE; I != IE; ++I) {
    std::string name = I->path().filename().generic_string();

    if (!is_directory(I->status())) {
      if (name.starts_with("poolfrontend") && name.ends_with(".log"))
        continue;
      if (name == "migrate.log")
        continue;
      LOG_F(WARNING, "Unexpected file in database directory: %s", name.c_str());
      continue;
    }

    if (name.starts_with("__"))
      continue;
    if (isNonCoinDirectory(name))
      continue;

    CCoinInfo info = CCoinLibrary::get(name.c_str());
    if (info.Name.empty()) {
      for (const auto &proc : gPluginContext.AddExtraCoinProcs) {
        if (proc(name.c_str(), info))
          break;
      }
    }

    if (info.Name.empty()) {
      LOG_F(WARNING, "Unknown directory in database path: %s", name.c_str());
      continue;
    }

    if (coinMap.count(info.Name))
      continue;

    CCoinInfoOld2 old2 = CCoinLibraryOld2::get(name.c_str());
    for (const auto &proc : gPluginContext.AddExtraCoinOld2Procs) {
      if (proc(name.c_str(), old2))
        break;
    }

    if (info.IsAlgorithm)
      LOG_F(INFO, "Found algorithm: %s", info.Name.c_str());
    else
      LOG_F(INFO, "Found coin: %s", info.Name.c_str());

    old2Map.emplace(info.Name, old2);
    coinMap.emplace(info.Name, std::move(info));
  }

  return true;
}

static bool migrateBalance(const std::filesystem::path &srcCoinPath, const std::filesystem::path &dstCoinPath, const CCoinInfo &coinInfo, const CCoinInfoOld2 &old2)
{
  return migrateDatabase(srcCoinPath, dstCoinPath, "balance", "balance.2", [&coinInfo, &old2](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    UserBalanceRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize balance record, database corrupted");
      return false;
    }

    UserBalanceRecord newRecord;
    newRecord.Login = oldRecord.Login;
    newRecord.Balance = fromRational(static_cast<uint64_t>(oldRecord.BalanceWithFractional));
    newRecord.Balance /= static_cast<uint64_t>(old2.ExtraMultiplier);
    newRecord.Requested = fromRational(static_cast<uint64_t>(oldRecord.Requested));
    newRecord.Paid = fromRational(static_cast<uint64_t>(oldRecord.Paid));

    LOG_F(INFO, "  %s balance=%s requested=%s paid=%s",
          oldRecord.Login.c_str(),
          FormatMoney(newRecord.Balance, coinInfo.FractionalPartSize).c_str(),
          FormatMoney(newRecord.Requested, coinInfo.FractionalPartSize).c_str(),
          FormatMoney(newRecord.Paid, coinInfo.FractionalPartSize).c_str());

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  });
}

static bool migrateStats(const std::filesystem::path &srcCoinPath, const std::filesystem::path &dstCoinPath, const CCoinInfoOld2 &old2, const char *baseName, const char *newName, unsigned threads, const std::string &cutoff)
{
  return migrateDatabaseMt(srcCoinPath, dstCoinPath, baseName, newName, [&old2](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    StatsRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize stats record, database corrupted");
      return false;
    }

    CWorkSummaryEntryWithTime newRecord;
    newRecord.UserId = oldRecord.Login;
    newRecord.WorkerId = oldRecord.WorkerId;
    // For migrated data: TimeBegin = TimeEnd (no interval info in old format)
    newRecord.Time = TimeInterval::point(Timestamp::fromUnixTime(oldRecord.Time));
    newRecord.Data.SharesNum = oldRecord.ShareCount;
    newRecord.Data.SharesWork = UInt<256>::fromDouble(old2.WorkMultiplier);
    newRecord.Data.SharesWork.mulfp(oldRecord.ShareWork);
    newRecord.Data.PrimePOWTarget = oldRecord.PrimePOWTarget;
    newRecord.Data.PrimePOWSharesNum = oldRecord.PrimePOWShareCount;

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  }, threads, [&cutoff](const std::string &partition) {
    return partition >= cutoff;
  });
}

static bool migratePPLNSPayouts(const std::filesystem::path &srcCoinPath, const std::filesystem::path &dstCoinPath, const CCoinInfoOld2 &old2, unsigned threads)
{
  return migrateDatabaseMt(srcCoinPath, dstCoinPath, "pplns.payouts", "pplns.payouts.2", [&old2](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    CPPLNSPayout2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize pplns.payouts record, database corrupted");
      return false;
    }

    CPPLNSPayout newRecord;
    newRecord.Login = oldRecord.Login;
    newRecord.RoundStartTime = Timestamp::fromUnixTime(oldRecord.RoundStartTime);
    newRecord.BlockHash = oldRecord.BlockHash;
    newRecord.BlockHeight = oldRecord.BlockHeight;
    newRecord.RoundEndTime = Timestamp::fromUnixTime(oldRecord.RoundEndTime);
    newRecord.PayoutValue = fromRational(static_cast<uint64_t>(oldRecord.PayoutValue));
    newRecord.RateToBTC = oldRecord.RateToBTC;
    newRecord.RateBTCToUSD = oldRecord.RateBTCToUSD;

    xmstream keyStream;
    newRecord.serializeKey(keyStream);
    xmstream valStream;
    newRecord.serializeValue(valStream);
    batch.Put(rocksdb::Slice(keyStream.data<const char>(), keyStream.sizeOf()),
              rocksdb::Slice(valStream.data<const char>(), valStream.sizeOf()));
    return true;
  }, threads);
}

static bool migratePayouts(const std::filesystem::path &srcCoinPath, const std::filesystem::path &dstCoinPath, unsigned threads)
{
  return migrateDatabaseMt(srcCoinPath, dstCoinPath, "payouts", "payouts.2", [](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    PayoutDbRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize payouts record, database corrupted");
      return false;
    }

    PayoutDbRecord newRecord;
    newRecord.UserId = oldRecord.UserId;
    newRecord.Time = oldRecord.Time;
    newRecord.Value = fromRational(static_cast<uint64_t>(oldRecord.Value));
    newRecord.TransactionId = oldRecord.TransactionId;
    newRecord.TransactionData = oldRecord.TransactionData;
    newRecord.Status = oldRecord.Status;
    newRecord.TxFee = fromRational(static_cast<uint64_t>(oldRecord.TxFee));

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  }, threads);
}

static bool migratePoolBalance(const std::filesystem::path &srcCoinPath, const std::filesystem::path &dstCoinPath, const CCoinInfoOld2 &old2, unsigned threads)
{
  return migrateDatabaseMt(srcCoinPath, dstCoinPath, "poolBalance", "poolBalance.2", [&old2](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    PoolBalanceRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize poolBalance record, database corrupted");
      return false;
    }

    PoolBalanceRecord newRecord;
    newRecord.Time = oldRecord.Time;
    newRecord.Balance = fromRational(static_cast<uint64_t>(oldRecord.BalanceWithFractional));
    newRecord.Balance /= static_cast<uint64_t>(old2.ExtraMultiplier);
    newRecord.Immature = fromRational(static_cast<uint64_t>(oldRecord.Immature));
    newRecord.Users = fromRational(static_cast<uint64_t>(oldRecord.Users));
    newRecord.Queued = fromRational(static_cast<uint64_t>(oldRecord.Queued));
    newRecord.ConfirmationWait = fromRational(static_cast<uint64_t>(oldRecord.ConfirmationWait));
    newRecord.Net = fromRational(static_cast<uint64_t>(oldRecord.Net));

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  }, threads);
}

static bool migrateFoundBlocks(const std::filesystem::path &srcCoinPath, const std::filesystem::path &dstCoinPath, const CCoinInfo &coinInfo, const CCoinInfoOld2 &old2, unsigned threads)
{
  std::atomic<unsigned> debugCount{0};
  return migrateDatabaseMt(srcCoinPath, dstCoinPath, "foundBlocks", "foundBlocks.2", [&coinInfo, &old2, &debugCount](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    FoundBlockRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize foundBlocks record, database corrupted");
      return false;
    }

    FoundBlockRecord newRecord;
    newRecord.Height = oldRecord.Height;
    newRecord.Hash = oldRecord.Hash;
    newRecord.Time = Timestamp::fromUnixTime(oldRecord.Time);
    newRecord.AvailableCoins = fromRational(static_cast<uint64_t>(oldRecord.AvailableCoins));
    newRecord.FoundBy = oldRecord.FoundBy;
    newRecord.ExpectedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
    newRecord.ExpectedWork.mulfp(oldRecord.ExpectedWork);
    newRecord.AccumulatedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
    newRecord.AccumulatedWork.mulfp(oldRecord.AccumulatedWork);
    newRecord.PublicHash = oldRecord.PublicHash;

    unsigned n = debugCount.fetch_add(1);
    if (n < 10) {
      LOG_F(INFO, "  height=%llu availableCoins=%s foundBy=%s",
            static_cast<unsigned long long>(newRecord.Height),
            FormatMoney(newRecord.AvailableCoins, coinInfo.FractionalPartSize).c_str(),
            newRecord.FoundBy.c_str());
    } else if (n == 10) {
      LOG_F(INFO, "  ... and more");
    }

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  }, threads);
}

static bool migrateRounds(const std::filesystem::path &srcCoinPath, const std::filesystem::path &dstCoinPath, const CCoinInfo &coinInfo, const CCoinInfoOld2 &old2)
{
  return migrateDatabase(srcCoinPath, dstCoinPath, "rounds.v2", "rounds.3", [&coinInfo, &old2](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    MiningRound2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize rounds record, database corrupted");
      return false;
    }

    MiningRound newRecord;
    newRecord.Height = oldRecord.Height;
    newRecord.BlockHash = oldRecord.BlockHash;
    newRecord.EndTime = Timestamp::fromUnixTime(oldRecord.EndTime);
    newRecord.StartTime = Timestamp::fromUnixTime(oldRecord.StartTime);
    newRecord.TotalShareValue = UInt<256>::fromDouble(old2.WorkMultiplier);
    newRecord.TotalShareValue.mulfp(oldRecord.TotalShareValue);
    newRecord.AvailableCoins = fromRational(static_cast<uint64_t>(oldRecord.AvailableCoins));
    newRecord.AvailableCoins /= static_cast<uint64_t>(old2.ExtraMultiplier);
    newRecord.FoundBy = oldRecord.FoundBy;
    newRecord.ExpectedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
    newRecord.ExpectedWork.mulfp(oldRecord.ExpectedWork);
    newRecord.AccumulatedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
    newRecord.AccumulatedWork.mulfp(oldRecord.AccumulatedWork);
    newRecord.TxFee = fromRational(static_cast<uint64_t>(oldRecord.TxFee));
    newRecord.PrimePOWTarget = oldRecord.PrimePOWTarget;

    // PendingConfirmation: in old code, unpayed rounds were detected by non-empty Payouts.
    // For deferred-reward coins (ETH) payouts are empty until confirmation, so those rounds
    // cannot be distinguished from already-processed ones. This is a known limitation.
    newRecord.PendingConfirmation = !oldRecord.Payouts.empty();
    // PPSValue / PPSBlockPart stay zero — PPS mode didn't exist before migration.

    for (const auto &s : oldRecord.UserShares) {
      UInt<256> shareValue = UInt<256>::fromDouble(old2.WorkMultiplier);
      shareValue.mulfp(s.ShareValue);
      UInt<256> incomingWork = UInt<256>::fromDouble(old2.WorkMultiplier);
      incomingWork.mulfp(s.IncomingWork);
      newRecord.UserShares.emplace_back(s.UserId, shareValue, incomingWork);
    }

    for (const auto &p : oldRecord.Payouts) {
      UInt<384> value = fromRational(static_cast<uint64_t>(p.Value));
      value /= static_cast<uint64_t>(old2.ExtraMultiplier);
      UInt<384> valueWithoutFee = fromRational(static_cast<uint64_t>(p.ValueWithoutFee));
      valueWithoutFee /= static_cast<uint64_t>(old2.ExtraMultiplier);
      UInt<256> acceptedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
      acceptedWork.mulfp(p.AcceptedWork);
      newRecord.Payouts.emplace_back(p.UserId, value, valueWithoutFee, acceptedWork);
    }

    LOG_F(INFO, "  height=%llu hash=%s endTime=%lld startTime=%lld totalShareValue=%s availableCoins=%s",
          static_cast<unsigned long long>(newRecord.Height),
          newRecord.BlockHash.c_str(),
          static_cast<long long>(newRecord.EndTime.toUnixTime()),
          static_cast<long long>(newRecord.StartTime.toUnixTime()),
          formatSI(newRecord.TotalShareValue.getDecimal()).c_str(),
          FormatMoney(newRecord.AvailableCoins, coinInfo.FractionalPartSize).c_str());
    LOG_F(INFO, "    foundBy=%s expectedWork=%s accumulatedWork=%s txFee=%s primePOWTarget=%u shares=%zu payouts=%zu",
          newRecord.FoundBy.c_str(),
          formatSI(newRecord.ExpectedWork.getDecimal()).c_str(),
          formatSI(newRecord.AccumulatedWork.getDecimal()).c_str(),
          FormatMoney(newRecord.TxFee, coinInfo.FractionalPartSize).c_str(),
          newRecord.PrimePOWTarget,
          newRecord.UserShares.size(),
          newRecord.Payouts.size());

    for (const auto &s : newRecord.UserShares) {
      LOG_F(INFO, "    * share %s shareValue=%s incomingWork=%s",
            s.UserId.c_str(),
            formatSI(s.ShareValue.getDecimal()).c_str(),
            formatSI(s.IncomingWork.getDecimal()).c_str());
    }

    for (const auto &p : newRecord.Payouts) {
      LOG_F(INFO, "    * payout %s value=%s valueWithoutFee=%s acceptedWork=%s",
            p.UserId.c_str(),
            FormatMoney(p.Value, coinInfo.FractionalPartSize).c_str(),
            FormatMoney(p.ValueWithoutFee, coinInfo.FractionalPartSize).c_str(),
            formatSI(p.AcceptedWork.getDecimal()).c_str());
    }

    // Strict checks on old data (before conversion)
    bool hasPerUserIncomingWork = false;
    if (!oldRecord.UserShares.empty()) {
      double oldShareSum = 0;
      double oldWorkSum = 0;
      for (const auto &s : oldRecord.UserShares) {
        oldShareSum += s.ShareValue;
        oldWorkSum += s.IncomingWork;
      }
      if (oldShareSum != oldRecord.TotalShareValue) {
        LOG_F(ERROR, "Round height=%llu: old data: sum of ShareValue (%.*g) != TotalShareValue (%.*g)",
              static_cast<unsigned long long>(oldRecord.Height), 17, oldShareSum, 17, oldRecord.TotalShareValue);
        return false;
      }

      hasPerUserIncomingWork = oldWorkSum != 0;
      // v1 rounds don't have per-user IncomingWork (all zeros), skip check
      if (hasPerUserIncomingWork && oldWorkSum != oldRecord.AccumulatedWork) {
        LOG_F(ERROR, "Round height=%llu: old data: sum of IncomingWork (%.*g) != AccumulatedWork (%.*g)",
              static_cast<unsigned long long>(oldRecord.Height), 17, oldWorkSum, 17, oldRecord.AccumulatedWork);
        return false;
      }
    }

    if (!oldRecord.Payouts.empty()) {
      int64_t oldPayoutSum = 0;
      for (const auto &p : oldRecord.Payouts)
        oldPayoutSum += p.Value;
      if (oldPayoutSum != oldRecord.AvailableCoins) {
        LOG_F(ERROR, "Round height=%llu: old data: sum of payout Value (%lld) != AvailableCoins (%lld)",
              static_cast<unsigned long long>(oldRecord.Height),
              static_cast<long long>(oldPayoutSum), static_cast<long long>(oldRecord.AvailableCoins));
        return false;
      }
    }

    // Correct rounding errors after conversion
    if (!newRecord.UserShares.empty()) {
      if (!correctSum(newRecord.UserShares, newRecord.TotalShareValue, newRecord.Height, "ShareValue",
            [](auto &s) -> UInt<256>& { return s.ShareValue; }))
        return false;
      // v1 rounds don't have per-user IncomingWork (all zeros), skip correction
      if (hasPerUserIncomingWork) {
        if (!correctSum(newRecord.UserShares, newRecord.AccumulatedWork, newRecord.Height, "IncomingWork",
              [](auto &s) -> UInt<256>& { return s.IncomingWork; }))
          return false;
      }
    }

    if (!newRecord.Payouts.empty()) {
      if (!correctSum(newRecord.Payouts, newRecord.AvailableCoins, newRecord.Height, "PayoutValue",
            [](auto &p) -> UInt<384>& { return p.Value; }))
        return false;
    }

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  });
}

// Helper: try to parse .dat file into CAccountingFileData
static bool tryParseAccountingDatFile(const std::filesystem::path &filePath, CAccountingFileData &fileData, const CCoinInfoOld2 *old2, int version)
{
  FileDescriptor fd;
  if (!fd.open(filePath.string().c_str())) {
    LOG_F(WARNING, "  Can't open file %s", filePath.string().c_str());
    return false;
  }

  size_t fileSize = fd.size();
  if (fileSize == 0)
    return false;

  xmstream stream(fileSize);
  size_t bytesRead = fd.read(stream.reserve(fileSize), 0, fileSize);
  fd.close();
  if (bytesRead != fileSize) {
    LOG_F(WARNING, "  Can't read file %s", filePath.string().c_str());
    return false;
  }
  stream.seekSet(0);

  if (version == 2) {
    // accounting.storage.2 - old format with double
    CAccountingFileDataOld2 oldData;
    DbIo<CAccountingFileDataOld2>::unserialize(stream, oldData);
    if (stream.eof())
      return false;

    fileData.LastShareId = oldData.LastShareId;
    fileData.LastBlockTime = oldData.LastBlockTime;
    fileData.Recent.resize(oldData.Recent.size());
    for (size_t i = 0; i < oldData.Recent.size(); i++) {
      fileData.Recent[i].UserId = std::move(oldData.Recent[i].UserId);
      fileData.Recent[i].Recent.resize(oldData.Recent[i].Recent.size());
      for (size_t j = 0; j < oldData.Recent[i].Recent.size(); j++) {
        fileData.Recent[i].Recent[j].SharesWork = UInt<256>::fromDouble(old2->WorkMultiplier);
        fileData.Recent[i].Recent[j].SharesWork.mulfp(oldData.Recent[i].Recent[j].SharesWork);
        fileData.Recent[i].Recent[j].Time = TimeInterval::point(Timestamp::fromUnixTime(oldData.Recent[i].Recent[j].TimeLabel));
      }
    }
    for (const auto &score : oldData.CurrentScores) {
      auto &v = fileData.CurrentScores.emplace(score.first, UInt<256>::fromDouble(old2->WorkMultiplier)).first->second;
      v.mulfp(score.second);
    }
  } else if (version == 1) {
    // accounting.storage - very old format, manual parsing
    DbIo<decltype(fileData.LastShareId)>::unserialize(stream, fileData.LastShareId);
    DbIo<decltype(fileData.LastBlockTime)>::unserialize(stream, fileData.LastBlockTime);
    // Recent stats
    VarSize recentSize;
    DbIo<VarSize>::unserialize(stream, recentSize);
    fileData.Recent.resize(recentSize.Size);
    for (uint64_t i = 0; i < recentSize.Size; i++) {
      DbIo<std::string>::unserialize(stream, fileData.Recent[i].UserId);
      VarSize entrySize;
      DbIo<VarSize>::unserialize(stream, entrySize);
      fileData.Recent[i].Recent.resize(entrySize.Size);
      for (uint64_t j = 0; j < entrySize.Size; j++) {
        uint32_t sharesNum;
        DbIo<uint32_t>::unserialize(stream, sharesNum);
        double sharesWork;
        DbIo<double>::unserialize(stream, sharesWork);
        int64_t oldTimeLabel;
        DbIo<int64_t>::unserialize(stream, oldTimeLabel);
        fileData.Recent[i].Recent[j].Time = TimeInterval::point(Timestamp::fromUnixTime(oldTimeLabel));
        fileData.Recent[i].Recent[j].SharesWork = UInt<256>::fromDouble(old2->WorkMultiplier);
        fileData.Recent[i].Recent[j].SharesWork.mulfp(sharesWork);
      }
    }
    // CurrentScores
    uint64_t scoresSize;
    DbIo<uint64_t>::unserialize(stream, scoresSize);
    for (uint64_t i = 0; i < scoresSize; i++) {
      std::string userId;
      double score;
      DbIo<std::string>::unserialize(stream, userId);
      DbIo<double>::unserialize(stream, score);
      auto &v = fileData.CurrentScores.emplace(userId, UInt<256>::fromDouble(old2->WorkMultiplier)).first->second;
      v.mulfp(score);
    }
  }

  if (stream.remaining() || stream.eof()) {
    LOG_F(WARNING, "  File %s is corrupted", filePath.string().c_str());
    return false;
  }

  return true;
}

// Helper: write CAccountingFileData to RocksDB
static bool writeAccountingStateToDb(rocksdbBase &stateDb, const CAccountingFileData &fileData, Timestamp currentRoundStartTime)
{
  rocksdbBase::CBatch batch = stateDb.batch("default");

  // Save LastShareId (lastmsgid) as 0 — new worklog uses separate message ID space
  {
    xmstream stream;
    DbIo<uint64_t>::serialize(stream, uint64_t{0});
    std::string key = "lastmsgid";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }

  // Save RecentStats
  {
    xmstream stream;
    DbIo<decltype(fileData.Recent)>::serialize(stream, fileData.Recent);
    std::string key = "recentstats";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }

  // Save CurrentScores
  {
    xmstream stream;
    DbIo<decltype(fileData.CurrentScores)>::serialize(stream, fileData.CurrentScores);
    std::string key = "currentscores";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }

  // Save CurrentRoundStartTime (if known)
  if (currentRoundStartTime != Timestamp()) {
    xmstream stream;
    DbIo<Timestamp>::serialize(stream, currentRoundStartTime);
    std::string key = "currentroundstart";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }

  return stateDb.writeBatch(batch);
}

// Migrate accounting storage from .dat files to RocksDB (accounting.state)
static bool migrateAccountingToState(const std::filesystem::path &srcCoinPath, const std::filesystem::path &dstCoinPath, const CCoinInfo &coinInfo, const CCoinInfoOld2 &old2)
{
  std::filesystem::path dstDbPath = dstCoinPath / "accounting.state";
  if (std::filesystem::exists(dstDbPath)) {
    LOG_F(INFO, "  accounting.state already exists, skipping migration");
    return true;
  }

  rocksdbBase stateDb(dstDbPath);

  // Migrate payouts.raw -> payoutqueue
  std::filesystem::path payoutsRawPath = srcCoinPath / "payouts.raw";
  if (std::filesystem::exists(payoutsRawPath)) {
    FileDescriptor fd;
    if (fd.open(payoutsRawPath.string().c_str())) {
      size_t fileSize = fd.size();
      if (fileSize > 0) {
        xmstream input;
        fd.read(input.reserve(fileSize), 0, fileSize);
        fd.close();
        input.seekSet(0);

        xmstream output;
        unsigned count = 0;
        while (input.remaining()) {
          PayoutDbRecord2 oldRecord;
          if (!oldRecord.deserializeValue(input)) {
            LOG_F(ERROR, "  Can't deserialize payouts.raw record, file corrupted");
            break;
          }

          PayoutDbRecord newRecord;
          newRecord.UserId = oldRecord.UserId;
          newRecord.Time = oldRecord.Time;
          newRecord.Value = fromRational(static_cast<uint64_t>(oldRecord.Value));
          newRecord.TransactionId = oldRecord.TransactionId;
          newRecord.TransactionData = oldRecord.TransactionData;
          newRecord.Status = oldRecord.Status;
          newRecord.TxFee = fromRational(static_cast<uint64_t>(oldRecord.TxFee));

          LOG_F(INFO, "    payout: %s value=%s txId=%s status=%u",
                newRecord.UserId.c_str(),
                FormatMoney(newRecord.Value, coinInfo.FractionalPartSize).c_str(),
                newRecord.TransactionId.c_str(), newRecord.Status);

          newRecord.serializeValue(output);
          count++;
        }

        if (output.sizeOf() > 0) {
          std::string key = "payoutqueue";
          stateDb.put("default", key.data(), key.size(), output.data(), output.sizeOf());
          LOG_F(INFO, "  Migrated %u payouts from payouts.raw", count);
        }
      }
    }
  }

  // Find EndTime of the last round (= start time of current round)
  Timestamp lastRoundEndTime;
  {
    std::filesystem::path roundsPath = dstCoinPath / "rounds.3";
    if (std::filesystem::exists(roundsPath)) {
      kvdb<rocksdbBase> roundsDb(roundsPath);
      std::unique_ptr<rocksdbBase::IteratorType> It(roundsDb.iterator());
      It->seekLast();
      if (It->valid()) {
        MiningRound R;
        RawData data = It->value();
        if (R.deserializeValue(data.data, data.size))
          lastRoundEndTime = R.EndTime;
      }
    }
  }

  // Load files from both directories
  std::deque<CDatFile> datFilesV2;
  std::deque<CDatFile> datFilesV1;
  enumerateDatFiles(datFilesV2, srcCoinPath / "accounting.storage.2", 2, false);
  enumerateDatFiles(datFilesV1, srcCoinPath / "accounting.storage", 1, false);

  // Combine and sort by FileId descending (newest first)
  std::vector<CDatFile> datFiles;
  datFiles.insert(datFiles.end(), std::make_move_iterator(datFilesV1.begin()), std::make_move_iterator(datFilesV1.end()));
  datFiles.insert(datFiles.end(), std::make_move_iterator(datFilesV2.begin()), std::make_move_iterator(datFilesV2.end()));
  std::sort(datFiles.begin(), datFiles.end(), [](const CDatFile &a, const CDatFile &b) { return a.FileId > b.FileId; });

  if (datFiles.empty()) {
    LOG_F(INFO, "  No accounting storage found to migrate");
    return true;
  }

  LOG_F(INFO, "  Migrating accounting state (%zu files found)", datFiles.size());

  // Try to parse files from newest to oldest
  for (auto &file : datFiles) {
    CAccountingFileData fileData;
    if (tryParseAccountingDatFile(file.Path, fileData, &old2, file.Version)) {
      LOG_F(INFO, "    Parsed %s: lastShareId=%llu recentUsers=%zu scores=%zu",
            file.Path.filename().string().c_str(),
            static_cast<unsigned long long>(fileData.LastShareId),
            fileData.Recent.size(), fileData.CurrentScores.size());

      if (writeAccountingStateToDb(stateDb, fileData, lastRoundEndTime)) {
        LOG_F(INFO, "    Successfully migrated to accounting.state");
        return true;
      } else {
        LOG_F(ERROR, "    Failed to write to accounting.state");
        return false;
      }
    }
  }

  LOG_F(WARNING, "    All .dat files are corrupted");
  return true;
}

static void serializeStatsFileData(xmstream &output, const CStatsFileData &fileData, uint64_t lastShareIdOverride = UINT64_MAX)
{
  uint64_t lastShareId = (lastShareIdOverride != UINT64_MAX) ? lastShareIdOverride : fileData.LastShareId;
  DbIo<CStatsFileData>::serializeHeader(output, lastShareId, fileData.Records.size());
  for (const auto &record : fileData.Records)
    DbIo<CWorkSummaryEntry>::serialize(output, record);
}

// stats cache v1 (manual format, no version header, no PrimePOW fields) -> v3
static bool migrateStatsCache(const std::filesystem::path &srcCoinPath, const std::filesystem::path &dstCoinPath, const char *baseName, const char *newName, const CCoinInfoOld2 &old2, unsigned threads, uint64_t lastShareIdOverride = UINT64_MAX)
{
  return migrateDirectoryMt(srcCoinPath, dstCoinPath, baseName, newName, [&old2, lastShareIdOverride](xmstream &input, xmstream &output) -> bool {
    CStatsFileData fileData;
    DbIo<decltype(fileData.LastShareId)>::unserialize(input, fileData.LastShareId);
    while (input.remaining()) {
      CWorkSummaryEntry &record = fileData.Records.emplace_back();
      uint32_t version;
      DbIo<decltype(version)>::unserialize(input, version);
      DbIo<decltype(record.UserId)>::unserialize(input, record.UserId);
      DbIo<decltype(record.WorkerId)>::unserialize(input, record.WorkerId);
      int64_t oldTime;
      DbIo<int64_t>::unserialize(input, oldTime);
      DbIo<decltype(record.Data.SharesNum)>::unserialize(input, record.Data.SharesNum);
      double shareWork;
      DbIo<double>::unserialize(input, shareWork);
      record.Data.SharesWork = UInt<256>::fromDouble(old2.WorkMultiplier);
      record.Data.SharesWork.mulfp(shareWork);
    }
    if (input.eof())
      return false;

    serializeStatsFileData(output, fileData, lastShareIdOverride);
    return true;
  }, threads, true);
}

// stats cache v2 (CStatsFileDataOld2 with double ShareWork) -> v3
static bool migrateStatsCacheV2(const std::filesystem::path &srcCoinPath, const std::filesystem::path &dstCoinPath, const char *baseName, const char *newName, const CCoinInfoOld2 &old2, unsigned threads, uint64_t lastShareIdOverride = UINT64_MAX)
{
  return migrateDirectoryMt(srcCoinPath, dstCoinPath, baseName, newName, [&old2, lastShareIdOverride](xmstream &input, xmstream &output) -> bool {
    CStatsFileDataOld2 oldData;
    DbIo<CStatsFileDataOld2>::unserialize(input, oldData);
    if (input.eof())
      return false;

    CStatsFileData fileData;
    fileData.LastShareId = oldData.LastShareId;
    for (const auto &oldRecord : oldData.Records) {
      CWorkSummaryEntry &record = fileData.Records.emplace_back();
      record.UserId = oldRecord.Login;
      record.WorkerId = oldRecord.WorkerId;
      record.Data.SharesNum = oldRecord.ShareCount;
      record.Data.SharesWork = UInt<256>::fromDouble(old2.WorkMultiplier);
      record.Data.SharesWork.mulfp(oldRecord.ShareWork);
      record.Data.PrimePOWTarget = oldRecord.PrimePOWTarget;
      record.Data.PrimePOWSharesNum.assign(oldRecord.PrimePOWShareCount.begin(), oldRecord.PrimePOWShareCount.end());
    }

    serializeStatsFileData(output, fileData, lastShareIdOverride);
    return true;
  }, threads, true);
}

// Read old share log v0 format (shares.log directory)
// Per record: uint64_t UniqueShareId, string userId, string workerId, double workValue, int64_t Time
// Note: UniqueShareId may not be monotonic due to a bug in old code, so no ordering check
static bool readOldShareLogV0(const std::filesystem::path &dirPath, std::vector<ParsedShare> &shares)
{
  if (!std::filesystem::exists(dirPath))
    return true;

  std::deque<CDatFile> files;
  enumerateDatFiles(files, dirPath, 0, false);
  if (files.empty())
    return true;

  LOG_F(INFO, "  Reading old share log v0 from %s (%zu files)", dirPath.string().c_str(), files.size());

  for (auto &file : files) {
    FileDescriptor fd;
    if (!fd.open(file.Path)) {
      LOG_F(ERROR, "  Can't open %s", file.Path.string().c_str());
      return false;
    }

    size_t fileSize = fd.size();
    if (fileSize == 0)
      continue;

    xmstream stream(fileSize);
    size_t bytesRead = fd.read(stream.reserve(fileSize), 0, fileSize);
    fd.close();
    if (bytesRead != fileSize) {
      LOG_F(ERROR, "  Can't read %s", file.Path.string().c_str());
      return false;
    }

    stream.seekSet(0);
    while (stream.remaining()) {
      ParsedShare share;
      share.UniqueShareId = stream.readle<uint64_t>();
      DbIo<std::string>::unserialize(stream, share.UserId);
      DbIo<std::string>::unserialize(stream, share.WorkerId);
      share.WorkValue = stream.read<double>();
      DbIo<int64_t>::unserialize(stream, share.Time);
      if (stream.eof()) {
        LOG_F(WARNING, "  Truncated file %s, read %zu shares before truncation", file.Path.string().c_str(), shares.size());
        break;
      }

      shares.push_back(std::move(share));
    }
  }

  return true;
}

// Read old share log v1 format (shares.log.v1 directory)
// Per record: ShareLogIo<CShareV1> (UniqueShareId is inside CShareV1)
// Note: UniqueShareId may not be monotonic due to a bug in old code, so no ordering check
static bool readOldShareLogV1(const std::filesystem::path &dirPath, std::vector<ParsedShare> &shares)
{
  if (!std::filesystem::exists(dirPath))
    return true;

  std::deque<CDatFile> files;
  enumerateDatFiles(files, dirPath, 1, false);
  if (files.empty())
    return true;

  LOG_F(INFO, "  Reading old share log v1 from %s (%zu files)", dirPath.string().c_str(), files.size());

  for (auto &file : files) {
    FileDescriptor fd;
    if (!fd.open(file.Path)) {
      LOG_F(ERROR, "  Can't open %s", file.Path.string().c_str());
      return false;
    }

    size_t fileSize = fd.size();
    if (fileSize == 0)
      continue;

    xmstream stream(fileSize);
    size_t bytesRead = fd.read(stream.reserve(fileSize), 0, fileSize);
    fd.close();
    if (bytesRead != fileSize) {
      LOG_F(ERROR, "  Can't read %s", file.Path.string().c_str());
      return false;
    }

    stream.seekSet(0);
    while (stream.remaining()) {
      CShareV1 oldShare;
      ShareLogIo<CShareV1>::unserialize(stream, oldShare);
      if (stream.eof()) {
        LOG_F(WARNING, "  Truncated file %s, read %zu shares before truncation", file.Path.string().c_str(), shares.size());
        break;
      }

      ParsedShare share;
      share.UniqueShareId = oldShare.UniqueShareId;
      share.UserId = std::move(oldShare.userId);
      share.WorkerId = std::move(oldShare.workerId);
      share.WorkValue = oldShare.WorkValue;
      share.Time = oldShare.Time;
      share.ChainLength = oldShare.ChainLength;
      share.PrimePOWTarget = oldShare.PrimePOWTarget;
      shares.push_back(std::move(share));
    }
  }

  return true;
}

// Get accounting threshold from old .dat files (max LastShareId across all parseable files)
static uint64_t getAccountingLastShareId(const std::filesystem::path &srcCoinPath, const CCoinInfoOld2 &old2)
{
  std::deque<CDatFile> datFiles;
  enumerateDatFiles(datFiles, srcCoinPath / "accounting.storage.2", 2, false);
  enumerateDatFiles(datFiles, srcCoinPath / "accounting.storage", 1, false);

  uint64_t maxShareId = 0;
  for (auto &file : datFiles) {
    CAccountingFileData fileData;
    if (tryParseAccountingDatFile(file.Path, fileData, &old2, file.Version))
      maxShareId = std::max(maxShareId, fileData.LastShareId);
  }

  return maxShareId;
}

// Get max LastShareId from stats cache .dat files
static uint64_t getStatsCacheMaxShareId(const std::filesystem::path &srcCoinPath, const char *cacheName)
{
  uint64_t maxShareId = 0;

  // v1 format: first field is uint64_t LastShareId (no version header)
  {
    std::deque<CDatFile> files;
    enumerateDatFiles(files, srcCoinPath / cacheName, 1, false);
    for (auto &file : files) {
      FileDescriptor fd;
      if (!fd.open(file.Path))
        continue;
      size_t fileSize = fd.size();
      if (fileSize < sizeof(uint64_t)) {
        fd.close();
        continue;
      }
      uint64_t lastShareId = 0;
      fd.read(&lastShareId, 0, sizeof(lastShareId));
      fd.close();
      maxShareId = std::max(maxShareId, lastShareId);
    }
  }

  // v2 format: uint32_t version + uint64_t LastShareId
  {
    std::string cacheName2 = std::string(cacheName) + ".2";
    std::deque<CDatFile> files;
    enumerateDatFiles(files, srcCoinPath / cacheName2, 2, false);
    for (auto &file : files) {
      FileDescriptor fd;
      if (!fd.open(file.Path))
        continue;
      size_t fileSize = fd.size();
      if (fileSize < sizeof(uint32_t) + sizeof(uint64_t)) {
        fd.close();
        continue;
      }
      uint32_t version = 0;
      fd.read(&version, 0, sizeof(version));
      uint64_t lastShareId = 0;
      fd.read(&lastShareId, sizeof(version), sizeof(lastShareId));
      fd.close();
      maxShareId = std::max(maxShareId, lastShareId);
    }
  }

  return maxShareId;
}

// Find the index in shares array where aggregation should start.
// Scans for exact UniqueShareId match; returns index after the found share.
// Returns nullopt if threshold not found (0 means no .dat files, >0 means ID not in log).
static std::optional<size_t> findThresholdIndex(const std::vector<ParsedShare> &shares, uint64_t threshold, const char *label)
{
  if (threshold == 0) {
    LOG_F(INFO, "    %s: no threshold in .dat files, skipping migration", label);
    return std::nullopt;
  }

  for (size_t i = 0; i < shares.size(); i++) {
    if (shares[i].UniqueShareId == threshold) {
      LOG_F(INFO, "    %s: threshold=%" PRIu64 " found at index %zu, will aggregate %zu shares",
            label, threshold, i, shares.size() - i - 1);
      return i + 1;
    }
  }

  LOG_F(WARNING, "    %s: threshold=%" PRIu64 " not found in share log, skipping migration",
        label, threshold);
  return std::nullopt;
}

// Aggregate parsed shares [beginIdx, endIdx) into CWorkSummaryEntry (for statistics)
static void aggregateStatsEntries(const std::vector<ParsedShare> &shares, size_t beginIdx, size_t endIdx,
                                  const CCoinInfoOld2 &old2,
                                  CWorkSummaryBatch &batch)
{
  // Key: UserId + '\0' + WorkerId
  struct WorkerAcc {
    uint64_t SharesNum = 0;
    UInt<256> SharesWork;
    uint32_t PrimePOWTarget = UINT32_MAX;
    std::vector<uint64_t> PrimePOWSharesNum;
  };

  std::map<std::string, WorkerAcc> acc;
  int64_t batchMinTime = INT64_MAX, batchMaxTime = INT64_MIN;

  for (size_t i = beginIdx; i < endIdx; i++) {
    const auto &share = shares[i];
    std::string key = share.UserId + '\0' + share.WorkerId;
    WorkerAcc &wa = acc[key];
    wa.SharesNum++;
    UInt<256> work = UInt<256>::fromDouble(old2.WorkMultiplier);
    work.mulfp(share.WorkValue);
    wa.SharesWork += work;
    batchMinTime = std::min(batchMinTime, share.Time);
    batchMaxTime = std::max(batchMaxTime, share.Time);
    if (share.PrimePOWTarget != 0)
      wa.PrimePOWTarget = std::min(wa.PrimePOWTarget, share.PrimePOWTarget);
    if (share.ChainLength > 0) {
      if (wa.PrimePOWSharesNum.size() < share.ChainLength)
        wa.PrimePOWSharesNum.resize(share.ChainLength, 0);
      wa.PrimePOWSharesNum[share.ChainLength - 1]++;
    }
  }

  if (!acc.empty())
    batch.Time = TimeInterval(Timestamp::fromUnixTime(batchMinTime), Timestamp::fromUnixTime(batchMaxTime));

  for (const auto &[key, wa] : acc) {
    CWorkSummaryEntry &entry = batch.Entries.emplace_back();
    size_t sep = key.find('\0');
    entry.UserId = key.substr(0, sep);
    entry.WorkerId = key.substr(sep + 1);
    entry.Data.SharesNum = wa.SharesNum;
    entry.Data.SharesWork = wa.SharesWork;
    entry.Data.PrimePOWTarget = (wa.PrimePOWTarget == UINT32_MAX) ? 0 : wa.PrimePOWTarget;
    entry.Data.PrimePOWSharesNum = wa.PrimePOWSharesNum;
  }
}

// Aggregate parsed shares [beginIdx, endIdx) into CUserWorkSummaryBatch (for accounting)
static void aggregateAccountingEntries(const std::vector<ParsedShare> &shares, size_t beginIdx, size_t endIdx,
                                       const CCoinInfoOld2 &old2,
                                       CUserWorkSummaryBatch &batch)
{
  struct UserAcc {
    UInt<256> AcceptedWork;
    uint64_t SharesNum = 0;
    int64_t MaxTime = INT64_MIN;
  };

  std::map<std::string, UserAcc> acc;
  int64_t batchMinTime = INT64_MAX, batchMaxTime = INT64_MIN;
  for (size_t i = beginIdx; i < endIdx; i++) {
    const auto &share = shares[i];
    UserAcc &ua = acc[share.UserId];
    ua.SharesNum++;
    UInt<256> work = UInt<256>::fromDouble(old2.WorkMultiplier);
    work.mulfp(share.WorkValue);
    ua.AcceptedWork += work;
    ua.MaxTime = std::max(ua.MaxTime, share.Time);
    batchMinTime = std::min(batchMinTime, share.Time);
    batchMaxTime = std::max(batchMaxTime, share.Time);
  }

  if (!acc.empty())
    batch.Time = TimeInterval(Timestamp::fromUnixTime(batchMinTime), Timestamp::fromUnixTime(batchMaxTime));

  for (const auto &[userId, ua] : acc) {
    CUserWorkSummary &entry = batch.Entries.emplace_back();
    entry.UserId = userId;
    entry.AcceptedWork = ua.AcceptedWork;
    entry.SharesNum = ua.SharesNum;
  }
}

// Compute summary statistics for a set of CWorkSummaryEntry
static void logStatsEntrySummary(const char *label, const std::vector<CWorkSummaryEntry> &entries)
{
  if (entries.empty()) {
    LOG_F(INFO, "    %s: empty", label);
    return;
  }
  uint64_t totalShares = 0;
  UInt<256> totalWork;
  for (const auto &e : entries) {
    totalShares += e.Data.SharesNum;
    totalWork += e.Data.SharesWork;
  }
  LOG_F(INFO, "    %s: %zu entries, %" PRIu64 " shares, work=%s",
        label, entries.size(), totalShares, formatSI(totalWork.getDecimal()).c_str());
}

// Compute summary statistics for a set of CUserWorkSummary
static void logAccountingEntrySummary(const char *label, const std::vector<CUserWorkSummary> &entries)
{
  if (entries.empty()) {
    LOG_F(INFO, "    %s: empty", label);
    return;
  }
  uint64_t totalShares = 0;
  UInt<256> totalWork;
  for (const auto &e : entries) {
    totalShares += e.SharesNum;
    totalWork += e.AcceptedWork;
  }
  LOG_F(INFO, "    %s: %zu users, %" PRIu64 " shares, work=%s",
        label, entries.size(), totalShares, formatSI(totalWork.getDecimal()).c_str());
}

// Migrate old ShareLog (shares.log / shares.log.v1) to new worklog format
// UniqueShareId is not monotonic due to a bug in old code, so we find threshold
// positions by exact match and aggregate shares sequentially after each position.
static bool migrateShareLogToWorklog(const std::filesystem::path &srcCoinPath,
                                     const std::filesystem::path &dstCoinPath,
                                     const CCoinInfoOld2 &old2,
                                     uint64_t *poolCacheLastShareId,
                                     uint64_t *workersCacheLastShareId)
{
  // Get thresholds from .dat files
  uint64_t accountingThreshold = getAccountingLastShareId(srcCoinPath, old2);
  uint64_t poolSaved = getStatsCacheMaxShareId(srcCoinPath, "stats.pool.cache");
  uint64_t workersSaved = getStatsCacheMaxShareId(srcCoinPath, "stats.workers.cache");

  LOG_F(INFO, "  ShareLog migration thresholds: accounting=%" PRIu64 " pool=%" PRIu64 " workers=%" PRIu64,
        accountingThreshold, poolSaved, workersSaved);

  // Read old shares (in file order, no monotonicity check)
  std::vector<ParsedShare> shares;

  if (!readOldShareLogV0(srcCoinPath / "shares.log", shares))
    return false;
  if (!readOldShareLogV1(srcCoinPath / "shares.log.v1", shares))
    return false;

  if (shares.empty()) {
    LOG_F(INFO, "  No old share log data to migrate");
    return true;
  }

  LOG_F(INFO, "  Read %zu shares from old share log", shares.size());

  // Find threshold positions by exact UniqueShareId match
  auto accountingStart = findThresholdIndex(shares, accountingThreshold, "accounting");
  auto poolStart = findThresholdIndex(shares, poolSaved, "pool");
  auto workersStart = findThresholdIndex(shares, workersSaved, "workers");

  // Aggregate and write statistics worklog
  if (poolStart && workersStart) {
    size_t statsMinStart = std::min(*poolStart, *workersStart);
    size_t statsMaxStart = std::max(*poolStart, *workersStart);

    CWorkSummaryBatch gapBatch, commonBatch;
    if (statsMinStart != statsMaxStart) {
      aggregateStatsEntries(shares, statsMinStart, statsMaxStart, old2, gapBatch);
      aggregateStatsEntries(shares, statsMaxStart, shares.size(), old2, commonBatch);
    } else {
      aggregateStatsEntries(shares, statsMinStart, shares.size(), old2, commonBatch);
    }

    std::filesystem::path statsWorklogDir = dstCoinPath / "statistic.worklog";
    std::filesystem::create_directories(statsWorklogDir);
    ShareLog<CWorkSummaryBatch> statsLog(statsWorklogDir, "migrate", INT64_MAX);
    statsLog.startLogging(1);

    if (statsMinStart != statsMaxStart) {
      // Gap exists: two messages (id=1 gap, id=2 common)
      statsLog.addMessage(gapBatch);
      statsLog.addMessage(commonBatch);

      // Override cache LastShareId: accumulator with lower start (more shares) replays both messages
      if (*poolStart < *workersStart) {
        *poolCacheLastShareId = 0;
        *workersCacheLastShareId = 1;
      } else {
        *poolCacheLastShareId = 1;
        *workersCacheLastShareId = 0;
      }
    } else {
      // No gap: single message (id=1)
      statsLog.addMessage(commonBatch);

      *poolCacheLastShareId = 0;
      *workersCacheLastShareId = 0;
    }

    statsLog.flush();

    LOG_F(INFO, "  Statistics worklog migration summary:");
    LOG_F(INFO, "    Stats pool: %zu shares (from index %zu)", shares.size() - *poolStart, *poolStart);
    LOG_F(INFO, "    Stats workers: %zu shares (from index %zu)", shares.size() - *workersStart, *workersStart);
    logStatsEntrySummary("Stats gap", gapBatch.Entries);
    logStatsEntrySummary("Stats common", commonBatch.Entries);
  } else {
    LOG_F(INFO, "  Skipping statistics worklog migration (threshold not found in share log)");
  }

  // Aggregate and write accounting worklog
  if (accountingStart) {
    CUserWorkSummaryBatch accountingBatch;
    aggregateAccountingEntries(shares, *accountingStart, shares.size(), old2, accountingBatch);

    std::filesystem::path accWorklogDir = dstCoinPath / "accounting.worklog";
    std::filesystem::create_directories(accWorklogDir);
    ShareLog<CUserWorkSummaryBatch> accLog(accWorklogDir, "migrate", INT64_MAX);
    accLog.startLogging(1);
    accLog.addMessage(accountingBatch);
    accLog.flush();

    LOG_F(INFO, "  Accounting worklog migration: %zu shares (from index %zu)", shares.size() - *accountingStart, *accountingStart);
    logAccountingEntrySummary("Accounting", accountingBatch.Entries);
  } else {
    LOG_F(INFO, "  Skipping accounting worklog migration (threshold not found in share log)");
  }

  return true;
}

static bool migrateCoin(const std::filesystem::path &srcPath, const std::filesystem::path &dstPath, const CCoinInfo &coinInfo, const CCoinInfoOld2 &old2, unsigned threads, const std::string &statisticCutoff)
{
  std::filesystem::path srcCoinPath = srcPath / coinInfo.Name;
  if (!std::filesystem::exists(srcCoinPath))
    return true;

  std::filesystem::path dstCoinPath = dstPath / coinInfo.Name;

  LOG_F(INFO, "Migrating %s: %s", coinInfo.IsAlgorithm ? "algorithm" : "coin", coinInfo.Name.c_str());

  if (!coinInfo.IsAlgorithm) {
    if (!migrateBalance(srcCoinPath, dstCoinPath, coinInfo, old2))
      return false;
  }

  if (!migratePoolBalance(srcCoinPath, dstCoinPath, old2, threads))
    return false;
  if (!migratePPLNSPayouts(srcCoinPath, dstCoinPath, old2, threads))
    return false;
  if (!migratePayouts(srcCoinPath, dstCoinPath, threads))
    return false;
  if (!migrateFoundBlocks(srcCoinPath, dstCoinPath, coinInfo, old2, threads))
    return false;
  if (!migrateStats(srcCoinPath, dstCoinPath, old2, "workerStats", "statistic", threads, statisticCutoff))
    return false;
  if (!migrateStats(srcCoinPath, dstCoinPath, old2, "poolstats", "statistic", threads, statisticCutoff))
    return false;
  if (!migrateRounds(srcCoinPath, dstCoinPath, coinInfo, old2))
    return false;

  // Migrate old ShareLog to new worklog format (must run before cache migration)
  uint64_t poolCacheLastShareId = UINT64_MAX;
  uint64_t workersCacheLastShareId = UINT64_MAX;
  if (!migrateShareLogToWorklog(srcCoinPath, dstCoinPath, old2, &poolCacheLastShareId, &workersCacheLastShareId))
    return false;

  if (!migrateAccountingToState(srcCoinPath, dstCoinPath, coinInfo, old2))
    return false;
  if (!migrateStatsCache(srcCoinPath, dstCoinPath, "stats.pool.cache", "stats.pool.cache.3", old2, threads, poolCacheLastShareId))
    return false;
  if (!migrateStatsCacheV2(srcCoinPath, dstCoinPath, "stats.pool.cache.2", "stats.pool.cache.3", old2, threads, poolCacheLastShareId))
    return false;
  if (!migrateStatsCache(srcCoinPath, dstCoinPath, "stats.workers.cache", "stats.workers.cache.3", old2, threads, workersCacheLastShareId))
    return false;
  if (!migrateStatsCacheV2(srcCoinPath, dstCoinPath, "stats.workers.cache.2", "stats.workers.cache.3", old2, threads, workersCacheLastShareId))
    return false;

  return true;
}

static bool migrateUserSettings(const std::filesystem::path &srcPath, const std::filesystem::path &dstPath, const std::unordered_map<std::string, CCoinInfo> &coinMap)
{
  return migrateDatabase(srcPath, dstPath, "usersettings", "usersettings.2", [&coinMap](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    UserSettingsRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize user settings record, database corrupted");
      return false;
    }

    UserSettingsRecord newRecord;
    newRecord.Login = oldRecord.Login;
    newRecord.Coin = oldRecord.Coin;
    newRecord.Address = oldRecord.Address;
    newRecord.AutoPayout = oldRecord.AutoPayout;
    newRecord.MinimalPayout = fromRational(static_cast<uint64_t>(oldRecord.MinimalPayout));

    auto coinIt = coinMap.find(oldRecord.Coin);
    if (coinIt == coinMap.end()) {
      LOG_F(WARNING, "Skipping user settings for unknown coin: %s (user: %s)", oldRecord.Coin.c_str(), oldRecord.Login.c_str());
      return true;  // Skip this record, continue with next
    }

    const CCoinInfo &coinInfo = coinIt->second;

    LOG_F(INFO, "  %s/%s address=%s minimalPayout=%s autoPayout=%u",
          oldRecord.Login.c_str(), oldRecord.Coin.c_str(), oldRecord.Address.c_str(),
          FormatMoney(newRecord.MinimalPayout, coinInfo.FractionalPartSize).c_str(), static_cast<unsigned>(oldRecord.AutoPayout));

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  });
}

bool migrateV3(const std::filesystem::path &srcPath, const std::filesystem::path &dstPath, unsigned threads, const std::string &statisticCutoff)
{
  std::unordered_map<std::string, CCoinInfo> coinMap;
  std::unordered_map<std::string, CCoinInfoOld2> old2Map;
  if (!loadCoinMap(srcPath, coinMap, old2Map))
    return false;

  if (!migrateUserSettings(srcPath, dstPath, coinMap))
    return false;

  for (const auto &[name, coinInfo] : coinMap) {
    if (!migrateCoin(srcPath, dstPath, coinInfo, old2Map[name], threads, statisticCutoff))
      return false;
  }

  // Copy non-coin databases (no format changes, just rewrite with zstd compression)
  static const char *copyDirs[] = {"useractions", "userfeeplan", "users", "usersessions"};
  for (const char *dir : copyDirs) {
    if (!copyDatabase(srcPath, dstPath, dir, threads))
      return false;
  }

  return true;
}
