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
#include "poolcommon/path.h"
#include "loguru.hpp"
#include <atomic>
#include <unordered_map>
#include <unordered_set>

// Legacy struct for migration from old .dat file format.
// Deserialization is handled manually in tryParseAccountingDatFile (version 1 and 2 paths),
// not via DbIo, because old binary format uses double/int64_t types that differ from new UInt types.
struct CAccountingFileData {
  enum { CurrentRecordVersion = 1 };

  uint64_t LastShareId;
  int64_t LastBlockTime;
  std::vector<CStatsExportData> Recent;
  std::map<std::string, UInt<256>> CurrentScores;
};

extern CPluginContext gPluginContext;

// Conversion from old int64_t rational values to new UInt<384> fixed-point.
// Negative values wrap around via UInt<384> overflow (two's complement).
static UInt<384> safeFromRational(int64_t value)
{
  if (value >= 0)
    return fromRational(static_cast<uint64_t>(value));
  return UInt<384>::zero() - fromRational(static_cast<uint64_t>(-value));
}

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
    CLOG_F(ERROR, "Round height={}: {} correction failed: sum={:d} expected={:d}",
          height, fieldName, sum, expected);
    return false;
  }

  CLOG_F(WARNING, "Round height={}: adjusted {} diff={:d} across {} items",
        height, fieldName, diff, items.size());
  return true;
}

class CPerThreadCollector {
public:
  explicit CPerThreadCollector(unsigned maxThreads)
    : sets_(maxThreads), generation_(nextGeneration_.fetch_add(1)) {}

  // Each thread inserts into its own set — no locking
  void insert(const std::string &value) {
    sets_[getSlot()].insert(value);
  }

  // After all threads complete — merge into target set
  void mergeInto(std::unordered_set<std::string> &target) {
    size_t count = nextSlot_.load();
    for (size_t i = 0; i < count; i++)
      target.insert(sets_[i].begin(), sets_[i].end());
  }

private:
  size_t getSlot() {
    thread_local size_t cachedGeneration = 0;
    thread_local size_t cachedSlot = 0;
    if (cachedGeneration != generation_) {
      cachedGeneration = generation_;
      cachedSlot = nextSlot_.fetch_add(1);
    }
    return cachedSlot;
  }

  std::vector<std::unordered_set<std::string>> sets_;
  std::atomic<size_t> nextSlot_{0};
  size_t generation_;
  static std::atomic<size_t> nextGeneration_;
};

std::atomic<size_t> CPerThreadCollector::nextGeneration_{1};


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
      CLOG_F(WARNING, "Unexpected file in database directory: {}", name);
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
      CLOG_F(WARNING, "Unknown directory in database path: {}", name);
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
      CLOG_F(INFO, "Found algorithm: {}", info.Name);
    else
      CLOG_F(INFO, "Found coin: {}", info.Name);

    old2Map.emplace(info.Name, old2);
    coinMap.emplace(info.Name, std::move(info));
  }

  return true;
}

static bool migrateStats(const std::filesystem::path &srcCoinPath, const std::filesystem::path &dstCoinPath, const CCoinInfoOld2 &old2, const char *baseName, const char *newName, unsigned threads, const std::string &cutoff)
{
  return migrateDatabaseMt(srcCoinPath, dstCoinPath, baseName, newName, [&old2](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    StatsRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      CLOG_F(ERROR, "Can't deserialize stats record, database corrupted");
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

static bool migrateStatistic(const std::filesystem::path &srcCoinPath,
                             const std::filesystem::path &dstCoinPath,
                             const CCoinInfoOld2 &old2,
                             unsigned threads,
                             const std::string &cutoff)
{
  if (!migrateStats(srcCoinPath, dstCoinPath, old2, "workerStats", "statistic", threads, cutoff))
    return false;
  if (!migrateStats(srcCoinPath, dstCoinPath, old2, "poolstats", "statistic", threads, cutoff))
    return false;
  return true;
}

static bool migratePPLNSPayouts(
  const std::filesystem::path &srcCoinPath,
  const std::filesystem::path &dstCoinPath,
  unsigned threads,
  const CPriceDatabase *priceDb,
  const std::string &coinGeckoName)
{
  std::atomic<unsigned> fixedCount{0};

  bool result = migrateDatabaseMt(
    srcCoinPath,
    dstCoinPath,
    "pplns.payouts",
    "pplns.payouts",
    [priceDb, &coinGeckoName, &fixedCount](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
      CPPLNSPayout2 oldRecord;
      if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
        CLOG_F(ERROR, "Can't deserialize pplns.payouts record, database corrupted");
        return false;
      }

      CPPLNSPayout newRecord;
      newRecord.Login = oldRecord.Login;
      newRecord.RoundStartTime = Timestamp::fromUnixTime(oldRecord.RoundStartTime);
      newRecord.BlockHash = oldRecord.BlockHash;
      newRecord.BlockHeight = oldRecord.BlockHeight;
      newRecord.RoundEndTime = Timestamp::fromUnixTime(oldRecord.RoundEndTime);
      newRecord.PayoutValue = safeFromRational(oldRecord.PayoutValue);
      if (priceDb && !coinGeckoName.empty()) {
        double btcPrice = priceDb->lookupPrice("bitcoin", oldRecord.RoundEndTime);
        if (btcPrice > 0.0) {
          if (coinGeckoName == "bitcoin") {
            newRecord.RateToBTC = 1.0;
            newRecord.RateBTCToUSD = btcPrice;
            fixedCount.fetch_add(1);
          } else {
            double coinPrice = priceDb->lookupPrice(coinGeckoName, oldRecord.RoundEndTime);
            if (coinPrice > 0.0) {
              newRecord.RateToBTC = coinPrice / btcPrice;
              newRecord.RateBTCToUSD = btcPrice;
              fixedCount.fetch_add(1);
            } else {
              newRecord.RateToBTC = oldRecord.RateToBTC;
              newRecord.RateBTCToUSD = oldRecord.RateBTCToUSD;
            }
          }
        } else {
          newRecord.RateToBTC = oldRecord.RateToBTC;
          newRecord.RateBTCToUSD = oldRecord.RateBTCToUSD;
        }
      } else {
        newRecord.RateToBTC = oldRecord.RateToBTC;
        newRecord.RateBTCToUSD = oldRecord.RateBTCToUSD;
      }

      xmstream keyStream;
      newRecord.serializeKey(keyStream);
      xmstream valStream;
      newRecord.serializeValue(valStream);
      batch.Put(
        rocksdb::Slice(keyStream.data<const char>(), keyStream.sizeOf()),
        rocksdb::Slice(valStream.data<const char>(), valStream.sizeOf()));
      return true;
    },
    threads);

  unsigned fixed = fixedCount.load();
  if (fixed > 0)
    CLOG_F(INFO, "  pplns.payouts: updated exchange rates in {} records", fixed);

  return result;
}

static bool migratePayouts(const std::filesystem::path &srcCoinPath,
                           const std::filesystem::path &dstCoinPath,
                           unsigned threads,
                           std::unordered_set<std::string> &activeUsers,
                           const CPriceDatabase *priceDb,
                           const std::string &coinGeckoName)
{
  CPerThreadCollector collector(threads);
  std::atomic<unsigned> fixedCount{0};
  bool result = migrateDatabaseMt(srcCoinPath, dstCoinPath, "payouts", "payouts",
    [&collector, priceDb, &coinGeckoName, &fixedCount](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    PayoutDbRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      CLOG_F(ERROR, "Can't deserialize payouts record, database corrupted");
      return false;
    }

    collector.insert(oldRecord.UserId);

    PayoutDbRecord newRecord;
    newRecord.UserId = oldRecord.UserId;
    newRecord.Time = Timestamp::fromUnixTime(oldRecord.Time);
    newRecord.Value = safeFromRational(oldRecord.Value);
    newRecord.TransactionId = oldRecord.TransactionId;
    newRecord.TransactionData = oldRecord.TransactionData;
    newRecord.Status = oldRecord.Status;
    newRecord.TxFee = safeFromRational(oldRecord.TxFee);
    if (priceDb && !coinGeckoName.empty()) {
      double btcPrice = priceDb->lookupPrice("bitcoin", oldRecord.Time);
      if (btcPrice > 0.0) {
        if (coinGeckoName == "bitcoin") {
          newRecord.RateToBTC = 1.0;
          newRecord.RateBTCToUSD = btcPrice;
          fixedCount.fetch_add(1);
        } else {
          double coinPrice = priceDb->lookupPrice(coinGeckoName, oldRecord.Time);
          if (coinPrice > 0.0) {
            newRecord.RateToBTC = coinPrice / btcPrice;
            newRecord.RateBTCToUSD = btcPrice;
            fixedCount.fetch_add(1);
          }
        }
      }
    }

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  }, threads);

  unsigned fixed = fixedCount.load();
  if (fixed > 0)
    CLOG_F(INFO, "  payouts: updated exchange rates in {} records", fixed);

  if (result)
    collector.mergeInto(activeUsers);
  return result;
}

static bool migratePoolBalance(const std::filesystem::path &srcCoinPath, const std::filesystem::path &dstCoinPath, const CCoinInfoOld2 &old2, unsigned threads)
{
  return migrateDatabaseMt(srcCoinPath, dstCoinPath, "poolBalance", "poolBalance", [&old2](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    PoolBalanceRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      CLOG_F(ERROR, "Can't deserialize poolBalance record, database corrupted");
      return false;
    }

    PoolBalanceRecord newRecord;
    newRecord.Time = oldRecord.Time;
    newRecord.Balance = safeFromRational(oldRecord.BalanceWithFractional);
    newRecord.Balance /= static_cast<uint64_t>(old2.ExtraMultiplier);
    newRecord.Immature = safeFromRational(oldRecord.Immature);
    newRecord.Users = safeFromRational(oldRecord.Users);
    newRecord.Queued = safeFromRational(oldRecord.Queued);
    newRecord.ConfirmationWait = safeFromRational(oldRecord.ConfirmationWait);
    newRecord.Net = safeFromRational(oldRecord.Net);

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  }, threads);
}

static bool migrateAllFoundBlocks(
  const std::filesystem::path &srcPath,
  const std::filesystem::path &dstPath,
  const std::unordered_map<std::string, CCoinInfo> &coinMap,
  const std::unordered_map<std::string, CCoinInfoOld2> &old2Map,
  std::unordered_set<std::string> &activeUsers)
{
  CLOG_F(INFO, "Migrating foundBlocks with merged mining cross-references ...");

  struct LoadedFoundBlock {
    std::string SerializedKey;
    FoundBlockRecord Record;
  };

  using PartitionKey = std::pair<std::string, std::string>;  // {coinName, partitionName}

  struct PartitionKeyHash {
    size_t operator()(const PartitionKey &k) const {
      size_t h = std::hash<std::string>{}(k.first);
      h ^= std::hash<std::string>{}(k.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  // All blocks grouped by partition from the start
  std::unordered_map<PartitionKey, std::vector<LoadedFoundBlock>, PartitionKeyHash> partitions;

  // Reference to a specific block: partition key + index within that partition's vector
  struct BlockRef {
    PartitionKey Partition;
    size_t Index;
  };

  std::unordered_map<std::string, std::vector<BlockRef>> hashIndex;
  size_t totalBlocks = 0;

  // Pass 1: Load all found blocks from all coins, grouped by partition
  for (const auto &[coinName, coinInfo] : coinMap) {
    std::filesystem::path oldDbPath = srcPath / coinName / "foundBlocks";
    if (!std::filesystem::exists(oldDbPath) || !std::filesystem::is_directory(oldDbPath))
      continue;

    const CCoinInfoOld2 &old2 = old2Map.at(coinName);
    unsigned coinBlockCount = 0;

    for (std::filesystem::directory_iterator I(oldDbPath), IE; I != IE; ++I) {
      if (!is_directory(I->status()))
        continue;

      std::string partitionName = I->path().filename().generic_string();
      PartitionKey partKey{coinName, partitionName};

      rocksdb::Options options;
      options.create_if_missing = false;
      rocksdb::DB *rawDb = nullptr;
      rocksdb::Status status = rocksdb::DB::Open(options, path_to_utf8(I->path()), &rawDb);
      if (!status.ok()) {
        CLOG_F(ERROR, "Can't open foundBlocks partition {}/{}: {}", coinName, partitionName, status.ToString());
        return false;
      }
      std::unique_ptr<rocksdb::DB> srcDb(rawDb);

      auto &partitionBlocks = partitions[partKey];

      rocksdb::ReadOptions readOptions;
      std::unique_ptr<rocksdb::Iterator> it(srcDb->NewIterator(readOptions));
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        FoundBlockRecord2 oldRecord;
        if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
          CLOG_F(ERROR, "Can't deserialize foundBlocks record for {}, database corrupted", coinName);
          return false;
        }

        activeUsers.insert(oldRecord.FoundBy);

        size_t idx = partitionBlocks.size();
        LoadedFoundBlock &block = partitionBlocks.emplace_back();
        block.SerializedKey.assign(it->key().data(), it->key().size());

        FoundBlockRecord &rec = block.Record;
        rec.Height = oldRecord.Height;
        rec.Hash = oldRecord.Hash;
        rec.Time = Timestamp::fromUnixTime(oldRecord.Time);
        rec.GeneratedCoins = safeFromRational(oldRecord.AvailableCoins);
        rec.FoundBy = oldRecord.FoundBy;
        rec.ExpectedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
        rec.ExpectedWork.mulfp(oldRecord.ExpectedWork);
        rec.AccumulatedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
        rec.AccumulatedWork.mulfp(oldRecord.AccumulatedWork);
        rec.PublicHash = oldRecord.PublicHash;

        if (coinBlockCount < 10) {
          CLOG_F(INFO, "  {}: height={} availableCoins={} foundBy={}",
                coinName, rec.Height, FormatMoney(rec.GeneratedCoins, coinInfo.FractionalPartSize), rec.FoundBy);
        } else if (coinBlockCount == 10) {
          CLOG_F(INFO, "  {}: ... and more", coinName);
        }
        coinBlockCount++;
        totalBlocks++;

        hashIndex[rec.Hash].push_back({partKey, idx});
      }
    }

    if (coinBlockCount > 0)
      CLOG_F(INFO, "  {}: loaded {} foundBlocks", coinName, coinBlockCount);
  }

  if (totalBlocks == 0) {
    CLOG_F(INFO, "  No foundBlocks to migrate");
    return true;
  }

  // Pass 2: Fill MergedBlocks for blocks sharing the same hash
  unsigned mergedCount = 0;
  for (const auto &[hash, refs] : hashIndex) {
    if (refs.size() < 2)
      continue;

    mergedCount++;
    for (const auto &ref : refs) {
      LoadedFoundBlock &block = partitions.at(ref.Partition)[ref.Index];
      for (const auto &other : refs) {
        if (&ref == &other)
          continue;
        const FoundBlockRecord &otherRecord = partitions.at(other.Partition)[other.Index].Record;
        CMergedBlockInfo info;
        info.CoinName = other.Partition.first;
        info.Height = otherRecord.Height;
        info.Hash = otherRecord.Hash;
        block.Record.MergedBlocks.push_back(std::move(info));
      }
    }
  }

  CLOG_F(INFO, "  Loaded {} blocks total, {} hashes shared across coins", totalBlocks, mergedCount);

  // Pass 3: Write each partition to destination database (no grouping needed)
  for (const auto &[partKey, blocks] : partitions) {
    const auto &[coinName, partitionName] = partKey;

    std::filesystem::path dstDbPath = dstPath / coinName / "foundBlocks";
    std::filesystem::create_directories(dstDbPath);

    std::filesystem::path dstPartPath = dstDbPath / partitionName;
    rocksdb::Options options;
    options.create_if_missing = true;
    options.compression = rocksdb::kZSTD;
    rocksdb::DB *rawDb = nullptr;
    rocksdb::Status status = rocksdb::DB::Open(options, path_to_utf8(dstPartPath), &rawDb);
    if (!status.ok()) {
      CLOG_F(ERROR, "Can't create foundBlocks partition {}/{}: {}", coinName, partitionName, status.ToString());
      return false;
    }
    std::unique_ptr<rocksdb::DB> dstDb(rawDb);

    rocksdb::WriteBatch batch;
    unsigned batchCount = 0;
    for (const auto &block : blocks) {
      xmstream stream;
      block.Record.serializeValue(stream);
      batch.Put(
        rocksdb::Slice(block.SerializedKey.data(), block.SerializedKey.size()),
        rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));

      batchCount++;
      if (batchCount >= MigrateBatchSize) {
        rocksdb::WriteOptions writeOptions;
        dstDb->Write(writeOptions, &batch);
        batch.Clear();
        batchCount = 0;
      }
    }

    if (batchCount > 0) {
      rocksdb::WriteOptions writeOptions;
      dstDb->Write(writeOptions, &batch);
    }

    CLOG_F(INFO, "  {}/{}: wrote {} records, compacting...", coinName, partitionName, blocks.size());
    rocksdb::CompactRangeOptions compactOptions;
    dstDb->CompactRange(compactOptions, nullptr, nullptr);
  }

  CLOG_F(INFO, "  Successfully migrated {} foundBlocks", totalBlocks);
  return true;
}

// Helper: try to parse .dat file into CAccountingFileData
static bool tryParseAccountingDatFile(const std::filesystem::path &filePath, CAccountingFileData &fileData, const CCoinInfoOld2 *old2, int version)
{
  FileDescriptor fd;
  if (!fd.open(filePath.string().c_str())) {
    CLOG_F(WARNING, "  Can't open file {}", filePath);
    return false;
  }

  size_t fileSize = fd.size();
  if (fileSize == 0)
    return false;

  xmstream stream(fileSize);
  size_t bytesRead = fd.read(stream.reserve(fileSize), 0, fileSize);
  fd.close();
  if (bytesRead != fileSize) {
    CLOG_F(WARNING, "  Can't read file {}", filePath);
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
    CLOG_F(WARNING, "  File {} is corrupted", filePath);
    return false;
  }

  return true;
}

// Migrate all accounting data to accounting.state + accounting.rounds
// Sources: rounds.v2, balance, accounting.storage*, payouts.raw
static bool migrateAccountingState(const std::filesystem::path &srcCoinPath,
                                   const std::filesystem::path &dstCoinPath,
                                   const CCoinInfo &coinInfo,
                                   const CCoinInfoOld2 &old2,
                                   std::unordered_set<std::string> &activeUsers,
                                   const CPriceDatabase *priceDb,
                                   const std::string &coinGeckoName)
{
  std::filesystem::path dstStatePath = dstCoinPath / "accounting.state";
  CLOG_F(INFO, "Migrating accounting state ...");

  // === 1. Read and convert rounds.v2 → completed + active ===
  std::vector<MiningRound> activeRounds;
  Timestamp lastRoundEndTime;
  uint64_t maxCompletedHeight = 0;
  {
    std::filesystem::path oldRoundsPath = srcCoinPath / "rounds.v2";
    if (std::filesystem::exists(oldRoundsPath) && std::filesystem::is_directory(oldRoundsPath)) {
      CLOG_F(INFO, "  Reading rounds.v2 ...");

      kvdb<rocksdbBase> roundsDb(dstCoinPath / "accounting.rounds");
      unsigned completedCount = 0;

      for (std::filesystem::directory_iterator I(oldRoundsPath), IE; I != IE; ++I) {
        if (!is_directory(I->status()))
          continue;

        rocksdb::Options options;
        options.create_if_missing = false;
        rocksdb::DB *rawDb = nullptr;
        rocksdb::Status status = rocksdb::DB::Open(options, path_to_utf8(I->path()), &rawDb);
        if (!status.ok()) {
          CLOG_F(ERROR, "Can't open partition {}: {}", I->path().filename(), status.ToString());
          return false;
        }
        std::unique_ptr<rocksdb::DB> srcDb(rawDb);

        rocksdb::ReadOptions readOptions;
        std::unique_ptr<rocksdb::Iterator> it(srcDb->NewIterator(readOptions));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
          MiningRound2 oldRecord;
          if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
            CLOG_F(ERROR, "Can't deserialize rounds record, database corrupted");
            return false;
          }

          MiningRound newRecord;
          newRecord.Block.Height = oldRecord.Height;
          newRecord.Block.Hash = oldRecord.BlockHash;
          newRecord.Block.Time = Timestamp::fromUnixTime(oldRecord.EndTime);
          newRecord.StartTime = Timestamp::fromUnixTime(oldRecord.StartTime);
          newRecord.TotalShareValue = UInt<256>::fromDouble(old2.WorkMultiplier);
          newRecord.TotalShareValue.mulfp(oldRecord.TotalShareValue);
          newRecord.Block.GeneratedCoins = safeFromRational(oldRecord.AvailableCoins);
          newRecord.Block.GeneratedCoins /= static_cast<uint64_t>(old2.ExtraMultiplier);
          newRecord.AvailableForPPLNS = newRecord.Block.GeneratedCoins;
          newRecord.Block.UserId = oldRecord.FoundBy;
          newRecord.Block.ExpectedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
          newRecord.Block.ExpectedWork.mulfp(oldRecord.ExpectedWork);
          newRecord.AccumulatedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
          newRecord.AccumulatedWork.mulfp(oldRecord.AccumulatedWork);
          newRecord.TxFee = safeFromRational(oldRecord.TxFee);
          newRecord.Block.PrimePOWTarget = oldRecord.PrimePOWTarget;

          // In old code, unpayed rounds were detected by non-empty Payouts.
          // For deferred-reward coins (ETH) payouts are empty until confirmation, so those rounds
          // cannot be distinguished from already-processed ones. This is a known limitation.
          bool isActive = !oldRecord.Payouts.empty();
          // PPSValue / PPSBlockPart stay zero — PPS mode didn't exist before migration.

          for (const auto &s : oldRecord.UserShares) {
            UInt<256> shareValue = UInt<256>::fromDouble(old2.WorkMultiplier);
            shareValue.mulfp(s.ShareValue);
            UInt<256> incomingWork = UInt<256>::fromDouble(old2.WorkMultiplier);
            incomingWork.mulfp(s.IncomingWork);
            newRecord.UserShares.emplace_back(s.UserId, shareValue, incomingWork);
          }

          for (const auto &p : oldRecord.Payouts) {
            UInt<384> value = safeFromRational(p.Value);
            value /= static_cast<uint64_t>(old2.ExtraMultiplier);
            UInt<384> valueWithoutFee = safeFromRational(p.ValueWithoutFee);
            valueWithoutFee /= static_cast<uint64_t>(old2.ExtraMultiplier);
            UInt<256> acceptedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
            acceptedWork.mulfp(p.AcceptedWork);
            newRecord.Payouts.emplace_back(p.UserId, value, valueWithoutFee, acceptedWork);
          }

          CLOG_F(INFO, "  height={} hash={} endTime={} startTime={} totalShareValue={} availableCoins={}",
                newRecord.Block.Height, newRecord.Block.Hash, newRecord.Block.Time.toUnixTime(),
                newRecord.StartTime.toUnixTime(), formatSI(newRecord.TotalShareValue.getDecimal()),
                FormatMoney(newRecord.AvailableForPPLNS, coinInfo.FractionalPartSize));
          CLOG_F(INFO,
                "    foundBy={} expectedWork={} accumulatedWork={} txFee={} primePOWTarget={} shares={} payouts={}"
                " pending={}",
                newRecord.Block.UserId, formatSI(newRecord.Block.ExpectedWork.getDecimal()),
                formatSI(newRecord.AccumulatedWork.getDecimal()),
                FormatMoney(newRecord.TxFee, coinInfo.FractionalPartSize),
                newRecord.Block.PrimePOWTarget, newRecord.UserShares.size(),
                newRecord.Payouts.size(), isActive);

          for (const auto &s : newRecord.UserShares) {
            CLOG_F(INFO, "    * share {} shareValue={} incomingWork={}",
                  s.UserId, formatSI(s.ShareValue.getDecimal()), formatSI(s.IncomingWork.getDecimal()));
          }

          for (const auto &p : newRecord.Payouts) {
            CLOG_F(INFO, "    * payout {} value={} valueWithoutFee={} acceptedWork={}",
                  p.UserId, FormatMoney(p.Value, coinInfo.FractionalPartSize),
                  FormatMoney(p.ValueWithoutFee, coinInfo.FractionalPartSize), formatSI(p.AcceptedWork.getDecimal()));
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
              CLOG_F(ERROR,
                    "Round height={}: old data: sum of ShareValue ({:.17g}) != TotalShareValue ({:.17g})",
                    oldRecord.Height, oldShareSum, oldRecord.TotalShareValue);
              return false;
            }

            hasPerUserIncomingWork = oldWorkSum != 0;
            // v1 rounds don't have per-user IncomingWork (all zeros), skip check
            if (hasPerUserIncomingWork && oldWorkSum != oldRecord.AccumulatedWork) {
              CLOG_F(ERROR,
                    "Round height={}: old data: sum of IncomingWork ({:.17g}) != AccumulatedWork ({:.17g})",
                    oldRecord.Height, oldWorkSum, oldRecord.AccumulatedWork);
              return false;
            }
          }

          if (!oldRecord.Payouts.empty()) {
            int64_t oldPayoutSum = 0;
            for (const auto &p : oldRecord.Payouts)
              oldPayoutSum += p.Value;
            if (oldPayoutSum != oldRecord.AvailableCoins) {
              CLOG_F(ERROR, "Round height={}: old data: sum of payout Value ({}) != AvailableCoins ({})",
                    oldRecord.Height, oldPayoutSum, oldRecord.AvailableCoins);
              return false;
            }
          }

          // Correct rounding errors after conversion
          if (!newRecord.UserShares.empty()) {
            if (!correctSum(newRecord.UserShares, newRecord.TotalShareValue, newRecord.Block.Height, "ShareValue",
                  [](auto &s) -> UInt<256>& { return s.ShareValue; }))
              return false;
            // v1 rounds don't have per-user IncomingWork (all zeros), skip correction
            if (hasPerUserIncomingWork) {
              if (!correctSum(newRecord.UserShares, newRecord.AccumulatedWork, newRecord.Block.Height, "IncomingWork",
                    [](auto &s) -> UInt<256>& { return s.IncomingWork; }))
                return false;
            }
          }

          if (!newRecord.Payouts.empty()) {
            if (!correctSum(newRecord.Payouts, newRecord.AvailableForPPLNS, newRecord.Block.Height, "PayoutValue",
                  [](auto &p) -> UInt<384>& { return p.Value; }))
              return false;
          }

          if (isActive) {
            activeRounds.push_back(std::move(newRecord));
          } else {
            // Track the highest-height completed round for lastRoundEndTime
            if (newRecord.Block.Height > maxCompletedHeight) {
              maxCompletedHeight = newRecord.Block.Height;
              lastRoundEndTime = newRecord.Block.Time;
            }
            roundsDb.put(newRecord);
            completedCount++;
          }
        }
      }

      CLOG_F(INFO, "  Migrated {} completed rounds to accounting.rounds, {} active rounds to state", completedCount, activeRounds.size());
    } else {
      CLOG_F(INFO, "  No previous rounds.v2 database found, skipping rounds");
    }
  }

  // Active rounds may have later EndTime than completed ones
  for (const auto &round : activeRounds) {
    if (round.Block.Time > lastRoundEndTime)
      lastRoundEndTime = round.Block.Time;
  }

  // === 2. Open accounting.state and prepare batch ===
  rocksdbBase stateDb(dstStatePath);
  rocksdbBase::CBatch batch = stateDb.batch("default");

  // === 3. Migrate balance records ===
  if (!coinInfo.IsAlgorithm) {
    std::filesystem::path oldBalancePath = srcCoinPath / "balance";
    if (std::filesystem::exists(oldBalancePath) && std::filesystem::is_directory(oldBalancePath)) {
      CLOG_F(INFO, "  Migrating balance -> accounting.state (b-prefix) ...");
      unsigned balanceCount = 0;

      rocksdbBase balanceDb(oldBalancePath);
      std::unique_ptr<rocksdbBase::IteratorType> it(balanceDb.iterator());
      it->seekFirst();
      for (; it->valid(); it->next()) {
        RawData data = it->value();
        UserBalanceRecord2 oldRecord;
        if (!oldRecord.deserializeValue(data.data, data.size)) {
          CLOG_F(ERROR, "Can't deserialize balance record, database corrupted");
          return false;
        }

        if (oldRecord.BalanceWithFractional != 0 || oldRecord.Requested != 0 || oldRecord.Paid != 0)
          activeUsers.insert(oldRecord.Login);

        UserBalanceRecord newRecord;
        newRecord.Login = oldRecord.Login;
        newRecord.Balance = safeFromRational(oldRecord.BalanceWithFractional);
        newRecord.Balance /= static_cast<uint64_t>(old2.ExtraMultiplier);
        newRecord.Requested = safeFromRational(oldRecord.Requested);
        newRecord.Paid = safeFromRational(oldRecord.Paid);

        CLOG_F(INFO, "  {} balance={} requested={} paid={}",
              oldRecord.Login, FormatMoney(newRecord.Balance, coinInfo.FractionalPartSize),
              FormatMoney(newRecord.Requested, coinInfo.FractionalPartSize),
              FormatMoney(newRecord.Paid, coinInfo.FractionalPartSize));

        batch.put(newRecord);
        balanceCount++;
      }

      if (balanceCount > 0)
        CLOG_F(INFO, "  Migrated {} balance records", balanceCount);
    } else {
      CLOG_F(INFO, "  No previous balance database found, skipping");
    }
  }

  // === 4. Migrate payouts.raw → .payoutqueue ===
  {
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
              CLOG_F(ERROR, "  Can't deserialize payouts.raw record, file corrupted");
              break;
            }

            PayoutDbRecord newRecord;
            newRecord.UserId = oldRecord.UserId;
            newRecord.Time = Timestamp::fromUnixTime(oldRecord.Time);
            newRecord.Value = safeFromRational(oldRecord.Value);
            newRecord.TransactionId = oldRecord.TransactionId;
            newRecord.TransactionData = oldRecord.TransactionData;
            newRecord.Status = oldRecord.Status;
            newRecord.TxFee = safeFromRational(oldRecord.TxFee);
            if (priceDb && !coinGeckoName.empty()) {
              double btcPrice = priceDb->lookupPrice("bitcoin", oldRecord.Time);
              if (btcPrice > 0.0) {
                if (coinGeckoName == "bitcoin") {
                  newRecord.RateToBTC = 1.0;
                  newRecord.RateBTCToUSD = btcPrice;
                } else {
                  double coinPrice = priceDb->lookupPrice(coinGeckoName, oldRecord.Time);
                  if (coinPrice > 0.0) {
                    newRecord.RateToBTC = coinPrice / btcPrice;
                    newRecord.RateBTCToUSD = btcPrice;
                  }
                }
              }
            }

            CLOG_F(INFO, "    payout: {} value={} txId={} status={}",
                  newRecord.UserId, FormatMoney(newRecord.Value, coinInfo.FractionalPartSize), newRecord.TransactionId, newRecord.Status);

            newRecord.serializeValue(output);
            count++;
          }

          if (output.sizeOf() > 0) {
            std::string key = ".payoutqueue";
            batch.put(key.data(), key.size(), output.data(), output.sizeOf());
            CLOG_F(INFO, "  Migrated {} payouts from payouts.raw", count);
          }
        }
      }
    }
  }

  // === 5. Migrate accounting .dat files → state metadata + active rounds ===
  {
    std::deque<CDatFile> datFilesV2;
    std::deque<CDatFile> datFilesV1;
    enumerateDatFiles(datFilesV2, srcCoinPath / "accounting.storage.2", 2, false);
    enumerateDatFiles(datFilesV1, srcCoinPath / "accounting.storage", 1, false);

    std::vector<CDatFile> datFiles;
    datFiles.insert(datFiles.end(),
                    std::make_move_iterator(datFilesV1.begin()),
                    std::make_move_iterator(datFilesV1.end()));
    datFiles.insert(datFiles.end(),
                    std::make_move_iterator(datFilesV2.begin()),
                    std::make_move_iterator(datFilesV2.end()));
    std::sort(datFiles.begin(), datFiles.end(),
              [](const CDatFile &a, const CDatFile &b) { return a.FileId > b.FileId; });

    if (!datFiles.empty()) {
      CLOG_F(INFO, "  Migrating accounting state ({} files found)", datFiles.size());

      bool parsed = false;
      for (auto &file : datFiles) {
        CAccountingFileData fileData;
        if (tryParseAccountingDatFile(file.Path, fileData, &old2, file.Version)) {
          CLOG_F(INFO, "    Parsed {}: lastShareId={} recentUsers={} scores={}",
                file.Path.filename(), fileData.LastShareId, fileData.Recent.size(), fileData.CurrentScores.size());

          // Save LastShareId as 0 — new worklog uses separate message ID space
          {
            xmstream stream;
            DbIo<uint64_t>::serialize(stream, uint64_t{0});
            std::string key = ".lastmsgid";
            batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
          }

          // Save RecentStats
          {
            xmstream stream;
            DbIo<decltype(fileData.Recent)>::serialize(stream, fileData.Recent);
            std::string key = ".recentstats";
            batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
          }

          // Save CurrentScores
          {
            xmstream stream;
            DbIo<decltype(fileData.CurrentScores)>::serialize(stream, fileData.CurrentScores);
            std::string key = ".currentscores";
            batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
          }

          // Save CurrentRoundStartTime (if known)
          if (lastRoundEndTime != Timestamp()) {
            xmstream stream;
            DbIo<Timestamp>::serialize(stream, lastRoundEndTime);
            std::string key = ".currentroundstart";
            batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
          }

          // Save active rounds
          for (const auto &round : activeRounds)
            batch.put(round);

          parsed = true;
          break;
        }
      }

      if (!parsed)
        CLOG_F(WARNING, "    All .dat files are corrupted");
    } else {
      CLOG_F(INFO, "  No accounting storage found to migrate");
    }
  }

  if (!stateDb.writeBatch(batch)) {
    CLOG_F(ERROR, "Failed to write to accounting.state");
    return false;
  }

  CLOG_F(INFO, "  Successfully migrated to accounting.state");
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
static bool migrateStatsCacheV1(const std::filesystem::path &srcCoinPath, const std::filesystem::path &dstCoinPath, const char *baseName, const char *newName, const CCoinInfoOld2 &old2, unsigned threads, uint64_t lastShareIdOverride = UINT64_MAX)
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

// Unified stats cache migration: v1 + v2 → v3
static bool migrateStatsCache(const std::filesystem::path &srcCoinPath,
                              const std::filesystem::path &dstCoinPath,
                              const char *baseName,
                              const CCoinInfoOld2 &old2,
                              unsigned threads,
                              uint64_t lastShareIdOverride = UINT64_MAX)
{
  std::string v2Name = std::string(baseName) + ".2";
  if (!migrateStatsCacheV1(srcCoinPath, dstCoinPath, baseName, baseName, old2, threads, lastShareIdOverride))
    return false;
  if (!migrateStatsCacheV2(srcCoinPath, dstCoinPath, v2Name.c_str(), baseName, old2, threads, lastShareIdOverride))
    return false;
  return true;
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

  CLOG_F(INFO, "  Reading old share log v0 from {} ({} files)", dirPath, files.size());

  for (auto &file : files) {
    FileDescriptor fd;
    if (!fd.open(file.Path)) {
      CLOG_F(ERROR, "  Can't open {}", file.Path);
      return false;
    }

    size_t fileSize = fd.size();
    if (fileSize == 0)
      continue;

    xmstream stream(fileSize);
    size_t bytesRead = fd.read(stream.reserve(fileSize), 0, fileSize);
    fd.close();
    if (bytesRead != fileSize) {
      CLOG_F(ERROR, "  Can't read {}", file.Path);
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
        CLOG_F(WARNING, "  Truncated file {}, read {} shares before truncation", file.Path, shares.size());
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

  CLOG_F(INFO, "  Reading old share log v1 from {} ({} files)", dirPath, files.size());

  for (auto &file : files) {
    FileDescriptor fd;
    if (!fd.open(file.Path)) {
      CLOG_F(ERROR, "  Can't open {}", file.Path);
      return false;
    }

    size_t fileSize = fd.size();
    if (fileSize == 0)
      continue;

    xmstream stream(fileSize);
    size_t bytesRead = fd.read(stream.reserve(fileSize), 0, fileSize);
    fd.close();
    if (bytesRead != fileSize) {
      CLOG_F(ERROR, "  Can't read {}", file.Path);
      return false;
    }

    stream.seekSet(0);
    while (stream.remaining()) {
      CShareV1 oldShare;
      ShareLogIo<CShareV1>::unserialize(stream, oldShare);
      if (stream.eof()) {
        CLOG_F(WARNING, "  Truncated file {}, read {} shares before truncation", file.Path, shares.size());
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
    CLOG_F(INFO, "    {}: no threshold in .dat files, skipping migration", label);
    return std::nullopt;
  }

  for (size_t i = 0; i < shares.size(); i++) {
    if (shares[i].UniqueShareId == threshold) {
      CLOG_F(INFO, "    {}: threshold={} found at index {}, will aggregate {} shares", label, threshold, i, shares.size() - i - 1);
      return i + 1;
    }
  }

  CLOG_F(WARNING, "    {}: threshold={} not found in share log, skipping migration", label, threshold);
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
    CLOG_F(INFO, "    {}: empty", label);
    return;
  }
  uint64_t totalShares = 0;
  UInt<256> totalWork;
  for (const auto &e : entries) {
    totalShares += e.Data.SharesNum;
    totalWork += e.Data.SharesWork;
  }
  CLOG_F(INFO, "    {}: {} entries, {} shares, work={}", label, entries.size(), totalShares, formatSI(totalWork.getDecimal()));
}

// Compute summary statistics for a set of CUserWorkSummary
static void logAccountingEntrySummary(const char *label, const std::vector<CUserWorkSummary> &entries)
{
  if (entries.empty()) {
    CLOG_F(INFO, "    {}: empty", label);
    return;
  }
  uint64_t totalShares = 0;
  UInt<256> totalWork;
  for (const auto &e : entries) {
    totalShares += e.SharesNum;
    totalWork += e.AcceptedWork;
  }
  CLOG_F(INFO, "    {}: {} users, {} shares, work={}", label, entries.size(), totalShares, formatSI(totalWork.getDecimal()));
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

  CLOG_F(INFO, "  ShareLog migration thresholds: accounting={} pool={} workers={}", accountingThreshold, poolSaved, workersSaved);

  // Read old shares (in file order, no monotonicity check)
  std::vector<ParsedShare> shares;

  if (!readOldShareLogV0(srcCoinPath / "shares.log", shares))
    return false;
  if (!readOldShareLogV1(srcCoinPath / "shares.log.v1", shares))
    return false;

  if (shares.empty()) {
    CLOG_F(INFO, "  No old share log data to migrate");
    return true;
  }

  CLOG_F(INFO, "  Read {} shares from old share log", shares.size());

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

    CLOG_F(INFO, "  Statistics worklog migration summary:");
    CLOG_F(INFO, "    Stats pool: {} shares (from index {})", shares.size() - *poolStart, *poolStart);
    CLOG_F(INFO, "    Stats workers: {} shares (from index {})", shares.size() - *workersStart, *workersStart);
    logStatsEntrySummary("Stats gap", gapBatch.Entries);
    logStatsEntrySummary("Stats common", commonBatch.Entries);
  } else {
    CLOG_F(INFO, "  Skipping statistics worklog migration (threshold not found in share log)");
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

    CLOG_F(INFO, "  Accounting worklog migration: {} shares (from index {})", shares.size() - *accountingStart, *accountingStart);
    logAccountingEntrySummary("Accounting", accountingBatch.Entries);
  } else {
    CLOG_F(INFO, "  Skipping accounting worklog migration (threshold not found in share log)");
  }

  return true;
}

static bool migrateCoin(const std::filesystem::path &srcPath,
                        const std::filesystem::path &dstPath,
                        const CCoinInfo &coinInfo,
                        const CCoinInfoOld2 &old2,
                        unsigned threads,
                        const std::string &statisticCutoff,
                        std::unordered_set<std::string> &activeUsers,
                        const CPriceDatabase *priceDb)
{
  std::filesystem::path srcCoinPath = srcPath / coinInfo.Name;
  if (!std::filesystem::exists(srcCoinPath))
    return true;

  std::filesystem::path dstCoinPath = dstPath / coinInfo.Name;

  CLOG_F(INFO, "Migrating {}: {}", coinInfo.IsAlgorithm ? "algorithm" : "coin", coinInfo.Name);

  // poolBalance/
  if (!migratePoolBalance(srcCoinPath, dstCoinPath, old2, threads))
    return false;
  // pplns.payouts/
  if (!migratePPLNSPayouts(srcCoinPath, dstCoinPath, threads, priceDb, coinInfo.CoinGeckoName))
    return false;
  // payouts/
  if (!migratePayouts(srcCoinPath, dstCoinPath, threads, activeUsers, priceDb, coinInfo.CoinGeckoName))
    return false;
  // statistic/  (workerStats + poolstats)
  if (!migrateStatistic(srcCoinPath, dstCoinPath, old2, threads, statisticCutoff))
    return false;
  // accounting.state/ + accounting.rounds/
  if (!migrateAccountingState(srcCoinPath, dstCoinPath, coinInfo, old2, activeUsers, priceDb, coinInfo.CoinGeckoName))
    return false;

  // statistic.worklog/ + accounting.worklog/  (must run before cache migration)
  uint64_t poolCacheLastShareId = UINT64_MAX;
  uint64_t workersCacheLastShareId = UINT64_MAX;
  if (!migrateShareLogToWorklog(srcCoinPath, dstCoinPath, old2, &poolCacheLastShareId, &workersCacheLastShareId))
    return false;

  // stats.pool.cache/  (v1 + v2 → current)
  if (!migrateStatsCache(srcCoinPath, dstCoinPath, "stats.pool.cache", old2, threads, poolCacheLastShareId))
    return false;
  // stats.workers.cache/  (v1 + v2 → current)
  if (!migrateStatsCache(srcCoinPath, dstCoinPath, "stats.workers.cache", old2, threads, workersCacheLastShareId))
    return false;

  // accounting.userstats/ — copy of users stats cache for the accounting subsystem.
  // Statistics and Accounting each maintain their own independent user stats accumulator
  // (CStatsSeriesMap), so after migration the same data must exist in both directories.
  {
    std::filesystem::path srcDir = dstCoinPath / "stats.users.cache";
    std::filesystem::path dstDir = dstCoinPath / "accounting.userstats";
    if (std::filesystem::exists(srcDir)) {
      std::filesystem::copy(srcDir, dstDir, std::filesystem::copy_options::recursive);
      CLOG_F(INFO, "  Copied stats.users.cache -> accounting.userstats");
    }
  }

  return true;
}

static bool migrateUserSettings(const std::filesystem::path &srcPath, const std::filesystem::path &dstPath, const std::unordered_map<std::string, CCoinInfo> &coinMap)
{
  return migrateDatabase(srcPath, dstPath, "usersettings", "usersettings", [&coinMap](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    UserSettingsRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      CLOG_F(ERROR, "Can't deserialize user settings record, database corrupted");
      return false;
    }

    UserSettingsRecord newRecord;
    newRecord.Login = oldRecord.Login;
    newRecord.Coin = oldRecord.Coin;
    newRecord.Payout.Address = oldRecord.Address;
    newRecord.Payout.Mode = EPayoutMode::Disabled;
    newRecord.Payout.InstantPayoutThreshold = safeFromRational(oldRecord.MinimalPayout);

    auto coinIt = coinMap.find(oldRecord.Coin);
    if (coinIt == coinMap.end()) {
      CLOG_F(WARNING, "Skipping user settings for unknown coin: {} (user: {})", oldRecord.Coin, oldRecord.Login);
      return true;  // Skip this record, continue with next
    }

    const CCoinInfo &coinInfo = coinIt->second;

    CLOG_F(INFO, "  {}/{} address={} instantPayoutThreshold={} mode=disabled",
          oldRecord.Login, oldRecord.Coin, oldRecord.Address,
          FormatMoney(newRecord.Payout.InstantPayoutThreshold, coinInfo.FractionalPartSize));

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  });
}

static bool migrateUsers(const std::filesystem::path &srcPath,
                         const std::filesystem::path &dstPath,
                         const std::unordered_set<std::string> &activeUsers)
{
  int64_t threeMonthsAgo = time(nullptr) - 90 * 24 * 3600;
  unsigned totalCount = 0;
  unsigned activeCount = 0;

  bool result = migrateDatabase(srcPath, dstPath, "users", "users", [&](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    UsersRecord oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      CLOG_F(ERROR, "Can't deserialize users record, database corrupted");
      return false;
    }

    totalCount++;

    bool isActive = activeUsers.count(oldRecord.Login) ||
                    oldRecord.RegistrationDate >= threeMonthsAgo;
    if (!isActive)
      return true;

    activeCount++;
    batch.Put(it->key(), it->value());
    return true;
  });

  CLOG_F(INFO, "Users: total={}, active={}, inactive (removed)={}", totalCount, activeCount, totalCount - activeCount);
  return result;
}

bool migrateV3(const std::filesystem::path &srcPath, const std::filesystem::path &dstPath, unsigned threads, const std::string &statisticCutoff, const std::filesystem::path &priceDbPath)
{
  CPriceDatabase priceDb;
  const CPriceDatabase *priceDbPtr = nullptr;
  if (!priceDbPath.empty()) {
    if (!loadPriceDatabase(priceDbPath, priceDb))
      return false;
    priceDbPtr = &priceDb;
  }

  std::unordered_map<std::string, CCoinInfo> coinMap;
  std::unordered_map<std::string, CCoinInfoOld2> old2Map;
  if (!loadCoinMap(srcPath, coinMap, old2Map))
    return false;

  if (!migrateUserSettings(srcPath, dstPath, coinMap))
    return false;

  std::unordered_set<std::string> activeUsers;
  for (const auto &[name, coinInfo] : coinMap) {
    if (!migrateCoin(srcPath, dstPath, coinInfo, old2Map[name], threads, statisticCutoff, activeUsers, priceDbPtr))
      return false;
  }

  // Migrate foundBlocks across all coins (with merged mining cross-references)
  if (!migrateAllFoundBlocks(srcPath, dstPath, coinMap, old2Map, activeUsers))
    return false;

  // Migrate users — copy only active users (have balance/payouts/blocks or registered recently)
  if (!migrateUsers(srcPath, dstPath, activeUsers))
    return false;

  // Copy non-coin databases (no format changes, just rewrite with zstd compression)
  static const char *copyDirs[] = {"useractions", "usersessions"};
  for (const char *dir : copyDirs) {
    if (!copyDatabase(srcPath, dstPath, dir, threads))
      return false;
  }

  // Create empty userfeeplan (old data is incompatible, start fresh)
  {
    std::filesystem::path feeplanPath = dstPath / "userfeeplan";
    std::filesystem::create_directories(feeplanPath);
    CLOG_F(INFO, "Created empty userfeeplan database (old data discarded)");
  }

  return true;
}
