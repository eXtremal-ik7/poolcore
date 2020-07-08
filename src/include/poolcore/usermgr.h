#pragma once

#include "kvdb.h"
#include "poolcore/rocksdbBase.h"
#include "asyncio/asyncio.h"
#include <tbb/concurrent_queue.h>
#include <thread>

class UserManager {
public:
  struct Credentials {
    std::string Login;
    std::string Password;
    std::string Name;
    std::string EMail;
    std::string TwoFactor;
  };

  struct UserInfo {
    std::string Name;
    std::string EMail;
  };

  struct UserCoinSettings {
    std::string Address;
    int64_t MinimalPayout;
    bool AutoPayoutEnabled;
  };

  // Asynchronous api
  class Task {
  public:
    virtual void run(UserManager *userMgr) = 0;
    virtual ~Task() {}
  };

  class UserCreateTask : public Task {
  public:
    UserCreateTask(Credentials &&credentials, std::function<void(bool, const std::string&)> callback) : Credentials_(credentials), Callback_(callback) {}
    void run(UserManager *userMgr) final {
      userMgr->userCreateImpl(Credentials_, Callback_);
    }
  private:
    Credentials Credentials_;
    std::function<void(bool, const std::string&)> Callback_;
  };

public:
  UserManager(const std::filesystem::path &dbPath);
  void start();
  void stop();

  // Asynchronous api
  void userCreate(Credentials &&credentials, std::function<void(bool, const std::string&)> callback) { startAsyncTask(new UserCreateTask(std::move(credentials), callback)); }

  // Synchronous api
  bool getUserInfo(const std::string &login, UserInfo &info);
  bool getUserCoinSettings(const std::string &login, const std::string &coin, UserCoinSettings &settings);

private:
  // Asynchronous api implementation
  void startAsyncTask(Task *task);
  void userCreateImpl(const Credentials &credentials, std::function<void(bool, const std::string&)> callback);

  void userManagerMain();

  asyncBase *Base_;
  aioUserEvent *TaskQueueEvent_;
  tbb::concurrent_queue<Task*> Tasks_;
  kvdb<rocksdbBase> UsersDb_;
  kvdb<rocksdbBase> UserSettingsDb_;
  std::thread Thread_;
};
