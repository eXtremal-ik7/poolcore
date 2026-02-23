#pragma once

#include "rocksdb/db.h"
#include "p2putils/xmstream.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>

static constexpr unsigned MigrateBatchSize = 1u << 14;

using MigrateCallback = std::function<bool(rocksdb::Iterator *it, rocksdb::WriteBatch &batch)>;
using MigrateFileCallback = std::function<bool(xmstream &input, xmstream &output)>;
using PartitionFilter = std::function<bool(const std::string &partitionName)>;

bool migrateDatabase(const std::filesystem::path &srcPath, const std::filesystem::path &dstPath, const char *baseName, const char *newName, MigrateCallback callback);
bool migrateDatabaseMt(const std::filesystem::path &srcPath, const std::filesystem::path &dstPath, const char *baseName, const char *newName, MigrateCallback callback, unsigned threads, PartitionFilter filter = nullptr);
bool migrateFile(const std::filesystem::path &srcDir, const std::filesystem::path &dstDir, const char *baseName, const char *newName, MigrateFileCallback callback);
bool migrateDirectory(const std::filesystem::path &srcPath, const std::filesystem::path &dstPath, const char *baseName, const char *newName, MigrateFileCallback callback, bool skipTargetCheck = false);
bool migrateDirectoryMt(const std::filesystem::path &srcPath, const std::filesystem::path &dstPath, const char *baseName, const char *newName, MigrateFileCallback callback, unsigned threads, bool skipTargetCheck = false);
bool copyDatabase(const std::filesystem::path &srcPath, const std::filesystem::path &dstPath, const char *name, unsigned threads);

std::string formatSI(const std::string &decimal);
bool isNonCoinDirectory(const std::string &name);

struct CPriceDatabase {
  // coinGeckoId → (unix_seconds → USD price)
  std::unordered_map<std::string, std::map<int64_t, double>> Prices;

  // Find closest price to timestamp. Returns 0.0 if no data.
  double lookupPrice(const std::string &coinGeckoId, int64_t timestamp) const;
};

bool loadPriceDatabase(const std::filesystem::path &dbPath, CPriceDatabase &out);
