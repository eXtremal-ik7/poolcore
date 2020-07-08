#include "poolcore/usermgr.h"
#include "loguru.hpp"

UserManager::UserManager(const std::filesystem::path &dbPath) :
  UsersDb_(dbPath / "users"),
  UserSettingsDb_(dbPath / "usersettings")
{
  Base_ = createAsyncBase(amOSDefault);
  TaskQueueEvent_ = newUserEvent(Base_, 0, [](aioUserEvent*, void *arg) {
    Task *task = nullptr;
    UserManager *userMgr = static_cast<UserManager*>(arg);
    while (userMgr->Tasks_.try_pop(task))
      task->run(userMgr);
  }, this);
}

void UserManager::start()
{
  Thread_ = std::thread([](UserManager *userMgr){ userMgr->userManagerMain(); }, this);
}

void UserManager::stop()
{
  postQuitOperation(Base_);
  Thread_.join();
}

void UserManager::userManagerMain()
{
  loguru::set_thread_name("UserManager");
  asyncLoop(Base_);
}

void UserManager::startAsyncTask(Task *task)
{
  Tasks_.push(task);
  userEventActivate(TaskQueueEvent_);
}

void UserManager::userCreateImpl(const Credentials &credentials, std::function<void(bool, const std::string&)> callback)
{
  LOG_F(ERROR, "userCreateImpl: not implemented");
  callback(false, "userCreateImpl: not implemented");
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
