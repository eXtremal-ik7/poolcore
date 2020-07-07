#include "poolcore/usermgr.h"

UserManager::UserManager(const std::filesystem::path &dbPath) :
  UsersDb_(dbPath / "users"),
  UserSettingsDb_(dbPath / "usersettings")
{

}

bool UserManager::getUserInfo(const std::string &login, UserInfo &info)
{
  // TODO: use rwlock
  return false;
}

bool UserManager::getUserCoinSettings(const std::string &login, const std::string &coin, UserCoinSettings &settings)
{
  // TODO: use rwlock
  return false;
}
