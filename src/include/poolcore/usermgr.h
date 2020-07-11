#pragma once

#include "kvdb.h"
#include "poolcore/backendData.h"
#include "poolcore/rocksdbBase.h"
#include "poolcore/uint256.h"
#include "asyncio/asyncio.h"
#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_hash_map.h>
#include <thread>
#include <unordered_set>

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

  // User session life time from last access (default: 30 minutes)
  static constexpr unsigned DefaultSessionLifeTime = 30*60;
  // User action(authentication data manage) life time (default 12 hours)
  static constexpr unsigned DefaultActionLifeTime = 12*60*60;
  // Users database cleanup interval (default: 10 minutes)
  static constexpr unsigned DefaultCleanupInterval = 10*60;

  // Asynchronous api
  class Task {
  public:
    using DefaultCb = std::function<void(bool, const std::string&)>;

    Task(UserManager *userMgr) : UserMgr_(userMgr) {}
    virtual void run() = 0;
    virtual ~Task() {}
  protected:
    UserManager *UserMgr_;
  };

  class UserActionTask : public Task {
  public:
    UserActionTask(UserManager *userMgr, const uint512 &actionId, DefaultCb callback) : Task(userMgr), ActionId_(actionId), Callback_(callback) {}
    void run() final { UserMgr_->actionImpl(ActionId_, Callback_); }
  private:
    uint512 ActionId_;
    DefaultCb Callback_;
  };

  class UserCreateTask : public Task {
  public:
    UserCreateTask(UserManager *userMgr, Credentials &&credentials, DefaultCb callback) : Task(userMgr), Credentials_(credentials), Callback_(callback) {}
    void run() final {
      coroutineTy *coroutine = coroutineNew([](void *arg) {
        auto task =  static_cast<UserCreateTask*>(arg);
        task->UserMgr_->userCreateImpl(task->Credentials_, task->Callback_);
      }, this, 0x10000);

      coroutineCall(coroutine);
    }

  private:
    Credentials Credentials_;
    DefaultCb Callback_;
  };

  class UserLoginTask: public Task {
  public:
    using Cb = std::function<void(const std::string&, const std::string&)>;
    UserLoginTask(UserManager *userMgr, Credentials &&credentials, Cb callback) : Task(userMgr), Credentials_(credentials), Callback_(callback) {}
    void run() final { UserMgr_->loginImpl(Credentials_, Callback_); }
  private:
    Credentials Credentials_;
    Cb Callback_;
  };

  class UserLogoutTask: public Task {
  public:
    UserLogoutTask(UserManager *userMgr, const uint512 sessionId, DefaultCb callback) : Task(userMgr), SessionId_(sessionId), Callback_(callback) {}
    void run() final { UserMgr_->logoutImpl(SessionId_, Callback_); }
  private:
    uint512 SessionId_;
    DefaultCb Callback_;
  };

public:
  UserManager(const std::filesystem::path &dbPath);
  void start();
  void stop();

  void setBaseCfg(const std::string &poolName,
                  const std::string &poolHostAddress,
                  const std::string &userActivateLinkPrefix) {
    BaseCfg.PoolName = poolName;
    BaseCfg.PoolHostAddress = poolHostAddress;
    BaseCfg.ActivateLinkPrefix = userActivateLinkPrefix;
  }

  void enableSMTP(HostAddress serverAddress,
                  const std::string &login,
                  const std::string &password,
                  const std::string &senderAddress,
                  bool useSmtps,
                  bool useStartTls) {
    SMTP.ServerAddress = serverAddress;
    SMTP.Login = login;
    SMTP.Password = password;
    SMTP.SenderAddress = senderAddress;
    SMTP.UseSmtps = useSmtps;
    SMTP.UseStartTls = useStartTls;
    SMTP.Enabled = true;
  }

  // Asynchronous api
  void userAction(const std::string &id, Task::DefaultCb callback) { startAsyncTask(new UserActionTask(this, uint512S(id), callback)); }
  void userCreate(Credentials &&credentials, Task::DefaultCb callback) { startAsyncTask(new UserCreateTask(this, std::move(credentials), callback)); }
  void userLogin(Credentials &&credentials, UserLoginTask::Cb callback) { startAsyncTask(new UserLoginTask(this, std::move(credentials), callback)); }
  void userLogout(const std::string &id, Task::DefaultCb callback) { startAsyncTask(new UserLogoutTask(this, uint512S(id), callback)); }

  // Synchronous api
  bool getUserInfo(const std::string &login, UserInfo &info);
  bool getUserCoinSettings(const std::string &login, const std::string &coin, UserCoinSettings &settings);

private:
  // Asynchronous api implementation
  void startAsyncTask(Task *task);
  void actionImpl(const uint512 &id, Task::DefaultCb callback);
  void userCreateImpl(Credentials &credentials, Task::DefaultCb callback);
  void loginImpl(Credentials &credentials, UserLoginTask::Cb callback);
  void logoutImpl(const uint512 &sessionId, Task::DefaultCb callback);

  void sessionAdd(const UserSessionRecord &sessionRecord, UsersRecord &usersRecord) {
    usersRecord.CurrentSessionId = sessionRecord.Id;
    SessionsCache_.insert(std::make_pair(sessionRecord.Id, sessionRecord));
    UserSessionsDb_.put(sessionRecord);
    UsersDb_.put(usersRecord);
  }

  void sessionRemove(const UserSessionRecord &sessionRecord, UsersRecord &usersRecord) {
    usersRecord.CurrentSessionId.SetNull();
    SessionsCache_.erase(sessionRecord.Id);
    UserSessionsDb_.deleteRow(sessionRecord);
    UsersDb_.put(usersRecord);
  }

  void actionAdd(const UserActionRecord &actionRecord, UsersRecord &usersRecord) {
    usersRecord.CurrentActionId = actionRecord.Id;
    ActionsCache_.insert(std::make_pair(actionRecord.Id, actionRecord));
    UserActionsDb_.put(actionRecord);
    UsersDb_.put(usersRecord);
  }

  void actionRemove(const UserActionRecord &actionRecord, UsersRecord &usersRecord) {
    usersRecord.CurrentActionId.SetNull();
    ActionsCache_.erase(actionRecord.Id);
    UserActionsDb_.deleteRow(actionRecord);
    UsersDb_.put(usersRecord);
  }

  void userManagerMain();
  void userManagerCleanup();

  asyncBase *Base_;
  aioUserEvent *TaskQueueEvent_;
  tbb::concurrent_queue<Task*> Tasks_;
  kvdb<rocksdbBase> UsersDb_;
  kvdb<rocksdbBase> UserSettingsDb_;
  kvdb<rocksdbBase> UserActionsDb_;
  kvdb<rocksdbBase> UserSessionsDb_;
  std::thread Thread_;

  aioUserEvent *CleanupEvent_;

  // Cached data structures
  // Concurrent access structures
  tbb::concurrent_hash_map<std::string, UsersRecord> UsersCache_;
  tbb::concurrent_hash_map<uint512, UserSessionRecord, TbbHash<512>> SessionsCache_;

  // Thread local structures
  std::unordered_map<uint512, UserActionRecord> ActionsCache_;
  std::unordered_set<std::string> AllEmails_;

  // Configuration
  struct {
    std::string PoolName;
    std::string PoolHostAddress;
    std::string ActivateLinkPrefix;
  } BaseCfg;

  // SMTP
  struct {
    HostAddress ServerAddress;
    std::string Login;
    std::string Password;
    std::string SenderAddress;
    bool UseSmtps;
    bool UseStartTls;
    bool Enabled = false;
  } SMTP;

  // Time intervals
  unsigned SessionLifeTime_ = DefaultSessionLifeTime;
  unsigned ActionLifeTime_ = DefaultActionLifeTime;
  unsigned CleanupInterval_ = DefaultCleanupInterval;
};
