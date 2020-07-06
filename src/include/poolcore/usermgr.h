#pragma once

#include "kvdb.h"
#include "poolcore/rocksdbBase.h"

class UsersDb {
public:
  UsersDb(const std::filesystem::path &dbPath);

private:
  kvdb<rocksdbBase> UsersDb_;
  kvdb<rocksdbBase> UserSettingsDb_;
};
