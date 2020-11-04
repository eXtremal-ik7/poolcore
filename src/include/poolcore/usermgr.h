#pragma once

#include "kvdb.h"
#include "poolcore/backendData.h"
#include "poolcore/poolCore.h"
#include "poolcore/rocksdbBase.h"
#include "poolcommon/uint256.h"
#include "asyncio/asyncio.h"
#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_hash_map.h>
#include <thread>
#include <unordered_set>

class UserManager {
public:
  enum ESpecialUser {
    ESpecialUserAdmin,
    ESpecialUserObserver
  };

  struct BackendParameters {
    int64_t DefaultMinimalPayout;
  };

  struct Credentials {
    std::string Login;
    std::string Password;
    std::string Name;
    std::string EMail;
    std::string TwoFactor;
    int64_t RegistrationDate;
    bool IsActive;
    bool IsReadOnly;
  };

  struct UserInfo {
    std::string Name;
    std::string EMail;
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
    using DefaultCb = std::function<void(const char*)>;

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

  class UserInitiateActionTask : public Task {
  public:
    UserInitiateActionTask(UserManager *userMgr, const std::string &login, UserActionRecord::EType type, DefaultCb callback) : Task(userMgr), Login_(login), Type_(type), Callback_(callback) {}
    void run() final {
      coroutineTy *coroutine = coroutineNew([](void *arg) {
        auto task = static_cast<UserInitiateActionTask*>(arg);
        task->UserMgr_->actionInitiateImpl(task->Login_, task->Type_, task->Callback_);
      }, this, 0x10000);

      coroutineCall(coroutine);
    }
  private:
    std::string Login_;
    UserActionRecord::EType Type_;
    DefaultCb Callback_;
  };

  class UserChangePasswordTask : public Task {
  public:
    UserChangePasswordTask(UserManager *userMgr, const uint512 &actionId, std::string newPassword, DefaultCb callback) : Task(userMgr), ActionId_(actionId), NewPassword_(newPassword), Callback_(callback) {}
    void run() final { UserMgr_->userChangePasswordImpl(ActionId_, NewPassword_, Callback_); }
  private:
    uint512 ActionId_;
    std::string NewPassword_;
    DefaultCb Callback_;
  };

  class UserCreateTask : public Task {
  public:
    UserCreateTask(UserManager *userMgr, Credentials &&credentials, DefaultCb callback, bool isActivated, bool isReadOnly) :
      Task(userMgr), Credentials_(credentials), Callback_(callback), IsActivated_(isActivated), IsReadOnly_(isReadOnly) {}
    void run() final {
      coroutineTy *coroutine = coroutineNew([](void *arg) {
        auto task =  static_cast<UserCreateTask*>(arg);
        task->UserMgr_->userCreateImpl(task->Credentials_, task->Callback_, task->IsActivated_, task->IsReadOnly_);
      }, this, 0x10000);

      coroutineCall(coroutine);
    }

  private:
    Credentials Credentials_;
    DefaultCb Callback_;
    bool IsActivated_;
    bool IsReadOnly_;
  };

  class UserResendEmailTask: public Task {
  public:
    UserResendEmailTask(UserManager *userMgr, Credentials &&credentials, DefaultCb callback) : Task(userMgr), Credentials_(credentials), Callback_(callback) {}
    void run() final {
      coroutineTy *coroutine = coroutineNew([](void *arg) {
        auto task =  static_cast<UserResendEmailTask*>(arg);
        task->UserMgr_->resendEmailImpl(task->Credentials_, task->Callback_);
      }, this, 0x10000);

      coroutineCall(coroutine);
    }
  private:
    Credentials Credentials_;
    DefaultCb Callback_;
  };

  class UserLoginTask: public Task {
  public:
    using Cb = std::function<void(const std::string&, const char*, bool)>;
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

  class UpdateSettingsTask: public Task {
  public:
    UpdateSettingsTask(UserManager *userMgr, UserSettingsRecord &&settings, DefaultCb callback) : Task(userMgr), Settings_(settings), Callback_(callback) {}
    void run() final { UserMgr_->updateSettingsImpl(Settings_, Callback_); }
  private:
    UserSettingsRecord Settings_;
    DefaultCb Callback_;
  };

  class EnumerateUsersTask: public Task {
  public:
    using Cb = std::function<void(std::vector<Credentials>&)>;
    EnumerateUsersTask(UserManager *userMgr, Cb callback) : Task(userMgr), Callback_(callback) {}
    void run() final { UserMgr_->enumerateUsersImpl(Callback_); }
  private:
    Cb Callback_;
  };

public:
  UserManager(const std::filesystem::path &dbPath);
  UserManager(const UserManager&) = delete;
  UserManager& operator=(const UserManager&) = delete;
  void start();
  void stop();

  static uint256 generateHash(const std::string &login, const std::string &password);

  void configAddCoin(const CCoinInfo &info, int64_t defaultMinimalPayout) {
    BackendParameters backendParameters;
    backendParameters.DefaultMinimalPayout = defaultMinimalPayout;
    CoinInfo_.push_back(info);
    BackendParameters_.push_back(backendParameters);
    CoinIdxMap_[info.Name] = CoinInfo_.size() - 1;
  }

  void setBaseCfg(const std::string &poolName,
                  const std::string &poolHostAddress,
                  const std::string &userActivateLinkPrefix,
                  const std::string &userChangePasswordLinkPrefix) {
    BaseCfg.PoolName = poolName;
    BaseCfg.PoolHostAddress = poolHostAddress;
    BaseCfg.ActivateLinkPrefix = userActivateLinkPrefix;
    BaseCfg.ChangePasswordLinkPrefix = userChangePasswordLinkPrefix;
  }

  void addSpecialUser(ESpecialUser type, const std::string &hash) {
    const char *name = nullptr;
    bool isReadOnly = false;
    switch (type) {
      case ESpecialUserAdmin :
        name = "admin";
        isReadOnly = false;
        break;
      case ESpecialUserObserver :
        name = "observer";
        isReadOnly = true;
        break;
    }

    if (name) {
      UsersRecord adminRecord;
      adminRecord.Login = name;
      adminRecord.PasswordHash = uint256S(hash);
      adminRecord.Name = name;
      adminRecord.IsActive = true;
      adminRecord.IsReadOnly = isReadOnly;
      UsersCache_.insert(std::make_pair(name, adminRecord));
    }
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

  std::vector<CCoinInfo> &coinInfo() { return CoinInfo_; }
  std::unordered_map<std::string, size_t> &coinIdxMap() { return CoinIdxMap_; }

  // Asynchronous api
  void userAction(const std::string &id, Task::DefaultCb callback) { startAsyncTask(new UserActionTask(this, uint512S(id), callback)); }
  void userActionInitiate(const std::string &login, UserActionRecord::EType type, Task::DefaultCb callback) { startAsyncTask(new UserInitiateActionTask(this, login, type, callback)); }
  void userChangePassword(const std::string &id, const std::string &newPassword, Task::DefaultCb callback) { startAsyncTask(new UserChangePasswordTask(this, uint512S(id), newPassword, callback)); }
  void userCreate(Credentials &&credentials, Task::DefaultCb callback, bool isActivated, bool isReadOnly) { startAsyncTask(new UserCreateTask(this, std::move(credentials), callback, isActivated, isReadOnly)); }
  void userResendEmail(Credentials &&credentials, Task::DefaultCb callback) { startAsyncTask(new UserResendEmailTask(this, std::move(credentials), callback)); }
  void userLogin(Credentials &&credentials, UserLoginTask::Cb callback) { startAsyncTask(new UserLoginTask(this, std::move(credentials), callback)); }
  void userLogout(const std::string &id, Task::DefaultCb callback) { startAsyncTask(new UserLogoutTask(this, uint512S(id), callback)); }
  void updateSettings(UserSettingsRecord &&settings, Task::DefaultCb callback) { startAsyncTask(new UpdateSettingsTask(this, std::move(settings), callback)); }
  void enumerateUsers(EnumerateUsersTask::Cb callback) { startAsyncTask(new EnumerateUsersTask(this, callback)); }

  // Synchronous api
  bool checkUser(const std::string &login);
  bool checkPassword(const std::string &login, const std::string &password);
  bool validateSession(const std::string &id, const std::string &targetLogin, std::string &resultLogin, bool needWriteAccess);
  bool getUserCredentials(const std::string &login, Credentials &out);
  bool getUserCoinSettings(const std::string &login, const std::string &coin, UserSettingsRecord &settings);

private:
  // Asynchronous api implementation
  void startAsyncTask(Task *task);
  void actionImpl(const uint512 &id, Task::DefaultCb callback);
  void actionInitiateImpl(const std::string &login, UserActionRecord::EType type, Task::DefaultCb callback);
  void userChangePasswordImpl(const uint512 &id, const std::string &newPassword, Task::DefaultCb callback);
  void userCreateImpl(Credentials &credentials, Task::DefaultCb callback, bool isActivated, bool isReadOnly);
  void resendEmailImpl(Credentials &credentials, Task::DefaultCb callback);
  void loginImpl(Credentials &credentials, UserLoginTask::Cb callback);
  void logoutImpl(const uint512 &sessionId, Task::DefaultCb callback);
  void updateSettingsImpl(const UserSettingsRecord &settings, Task::DefaultCb callback);
  void enumerateUsersImpl(EnumerateUsersTask::Cb callback);

  void sessionAdd(const UserSessionRecord &sessionRecord) {
    LoginSessionMap_[sessionRecord.Login] = sessionRecord.Id;
    SessionsCache_.insert(std::make_pair(sessionRecord.Id, sessionRecord));
    UserSessionsDb_.put(sessionRecord);
  }

  void sessionRemove(const UserSessionRecord &sessionRecord) {
    LoginSessionMap_.erase(sessionRecord.Login);
    UserSessionsDb_.deleteRow(sessionRecord);
    SessionsCache_.erase(sessionRecord.Id);
  }

  void actionAdd(const UserActionRecord &actionRecord) {
    LoginActionMap_[actionRecord.Login] = actionRecord.Id;
    ActionsCache_.insert(std::make_pair(actionRecord.Id, actionRecord));
    UserActionsDb_.put(actionRecord);
  }

  void actionRemove(const UserActionRecord &actionRecord) {
    LoginActionMap_.erase(actionRecord.Login);
    UserActionsDb_.deleteRow(actionRecord);
    ActionsCache_.erase(actionRecord.Id);
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
  tbb::concurrent_hash_map<std::string, UserSettingsRecord> SettingsCache_;

  // Thread local structures
  std::unordered_map<uint512, UserActionRecord> ActionsCache_;
  std::unordered_set<std::string> AllEmails_;
  std::map<std::string, uint512> LoginSessionMap_;
  std::map<std::string, uint512> LoginActionMap_;

  // Configuration
  std::vector<CCoinInfo> CoinInfo_;
  std::vector<BackendParameters> BackendParameters_;
  std::unordered_map<std::string, size_t> CoinIdxMap_;

  struct {
    std::string PoolName;
    std::string PoolHostAddress;
    std::string ActivateLinkPrefix;
    std::string ChangePasswordLinkPrefix;
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
