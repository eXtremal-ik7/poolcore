#pragma once

#include "rocksdb/db.h"
#include "p2putils/xmstream.h"
#include <filesystem>
#include <functional>
#include <string>

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
