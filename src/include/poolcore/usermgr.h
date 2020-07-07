#pragma once

#include "kvdb.h"
#include "poolcore/rocksdbBase.h"

class UserManager {
public:
  struct UserInfo {
    std::string Name;
    std::string EMail;
  };

  struct UserCoinSettings {
    std::string Address;
    int64_t MinimalPayout;
    bool AutoPayoutEnabled;
  };

public:
  UserManager(const std::filesystem::path &dbPath);

  bool getUserInfo(const std::string &login, UserInfo &info);
  bool getUserCoinSettings(const std::string &login, const std::string &coin, UserCoinSettings &settings);

private:
  kvdb<rocksdbBase> UsersDb_;
  kvdb<rocksdbBase> UserSettingsDb_;
};
