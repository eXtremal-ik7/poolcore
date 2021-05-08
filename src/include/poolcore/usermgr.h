#pragma once

#include "kvdb.h"
#include "poolcore/backendData.h"
#include "poolcore/poolCore.h"
#include "poolcore/rocksdbBase.h"
#include "poolcommon/intrusive_ptr.h"
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
    // Personal fee
    std::string ParentUser;
    double DefaultFee;
  };

  struct UserInfo {
    std::string Name;
    std::string EMail;
  };

  struct PersonalFeeNode {
    PersonalFeeNode *Parent;
    std::vector<PersonalFeeNode*> ChildNodes;
    std::string UserId;
    double DefaultFee;
    std::unordered_map<std::string, double> CoinSpecificFee;

    double getFeeCoefficient(const std::string &coinName) {
      if (CoinSpecificFee.empty())
        return DefaultFee;

      auto It = CoinSpecificFee.find(coinName);
      return It != CoinSpecificFee.end() ? It->second : DefaultFee;
    }

    void removeFromChildNodes(PersonalFeeNode *node) {
      auto It = std::find(ChildNodes.begin(), ChildNodes.end(), node);
      if (It != ChildNodes.end())
        ChildNodes.erase(It);
    }
  };

  class alignas(512) PersonalFeeTree {
  private:
    std::unordered_map<std::string, PersonalFeeNode*> Nodes_;
    std::atomic<uintptr_t> Refs_ = 0;

  public:
    ~PersonalFeeTree() {
      for (auto &node: Nodes_)
        delete node.second;
    }

    PersonalFeeNode *get(const std::string &userId) {
      auto It = Nodes_.find(userId);
      return It != Nodes_.end() ? It->second : nullptr;
    }

    PersonalFeeNode *getOrCreate(const std::string &userId) {
      auto It = Nodes_.find(userId);
      if (It != Nodes_.end()) {
        return It->second;
      } else {
        PersonalFeeNode *node = new PersonalFeeNode;
        node->Parent = nullptr;
        node->UserId = userId;
        node->DefaultFee = 0.0;
        Nodes_[userId] = node;
        return node;
      }
    }

    bool hasLoop() const {
      std::unordered_set<PersonalFeeNode*> knownNodes;
      for (const auto &It: Nodes_) {
        PersonalFeeNode *current = It.second;
        if (knownNodes.count(current))
          continue;

        std::unordered_set<PersonalFeeNode*> visited = {current};
        while (current->Parent && !knownNodes.count(current->Parent)) {
          if (!visited.insert(current->Parent).second)
            return true;
          current = current->Parent;
        }

        knownNodes.insert(visited.begin(), visited.end());
      }

      return false;
    }

    PersonalFeeTree *deepCopy() {
      PersonalFeeTree *result = new PersonalFeeTree;
      for (const auto &It: Nodes_) {
        PersonalFeeNode *oldNode = It.second;
        PersonalFeeNode *newNode = new PersonalFeeNode;
        newNode->ChildNodes.reserve(oldNode->ChildNodes.size());
        newNode->Parent = oldNode->Parent;
        newNode->UserId = oldNode->UserId;
        newNode->DefaultFee = oldNode->DefaultFee;
        newNode->CoinSpecificFee = oldNode->CoinSpecificFee;
        result->Nodes_[newNode->UserId] = newNode;

      }

      // Fix parent & child pointers
      for (auto &It: result->Nodes_) {
        PersonalFeeNode *node = It.second;
        if (node->Parent) {
          node->Parent = result->Nodes_[node->Parent->UserId];
          node->Parent->ChildNodes.push_back(node);
        }
      }

      return result;
    }

  public:
    uintptr_t ref_fetch_add(uint64_t value) { return Refs_.fetch_add(value); }
    uintptr_t ref_fetch_sub(uint64_t value) { return Refs_.fetch_sub(value); }
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
      coroutineTy *coroutine = UserMgr_->newCoroutine([](void *arg) {
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

  class UserChangePasswordForceTask : public Task {
  public:
    UserChangePasswordForceTask(UserManager *userMgr, const std::string &sessionId, const std::string &login, const std::string &newPassword, DefaultCb callback) : Task(userMgr), SessionId_(sessionId), Login_(login), NewPassword_(newPassword), Callback_(callback) {}
    void run() final { UserMgr_->userChangePasswordForceImpl(SessionId_, Login_, NewPassword_, Callback_); }
  private:
    std::string SessionId_;
    std::string Login_;
    std::string NewPassword_;
    DefaultCb Callback_;
  };

  class UserCreateTask : public Task {
  public:
    UserCreateTask(UserManager *userMgr, const std::string &login, Credentials &&credentials, DefaultCb callback) :
      Task(userMgr), Login_(login), Credentials_(credentials), Callback_(callback) {}
    void run() final {
      coroutineTy *coroutine = UserMgr_->newCoroutine([](void *arg) {
        auto task =  static_cast<UserCreateTask*>(arg);
        task->UserMgr_->userCreateImpl(task->Login_, task->Credentials_, task->Callback_);
      }, this, 0x10000);

      coroutineCall(coroutine);
    }

  private:
    std::string Login_;
    Credentials Credentials_;
    DefaultCb Callback_;
  };

  class UserResendEmailTask: public Task {
  public:
    UserResendEmailTask(UserManager *userMgr, Credentials &&credentials, DefaultCb callback) : Task(userMgr), Credentials_(credentials), Callback_(callback) {}
    void run() final {
      coroutineTy *coroutine = UserMgr_->newCoroutine([](void *arg) {
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

  class UpdateCredentialsTask: public Task {
  public:
    UpdateCredentialsTask(UserManager *userMgr, const std::string &sessionId, const std::string &targetLogin, Credentials &&credentials, DefaultCb callback) :
      Task(userMgr), SessionId_(sessionId), TargetLogin_(targetLogin), Credentials_(credentials), Callback_(callback) {}
    void run() final { UserMgr_->updateCredentialsImpl(SessionId_, TargetLogin_, Credentials_, Callback_); }
  private:
    std::string SessionId_;
    std::string TargetLogin_;
    Credentials Credentials_;
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
    using Cb = std::function<void(const char*, std::vector<Credentials>&)>;
    EnumerateUsersTask(UserManager *userMgr, const std::string &sessionId, Cb callback) : Task(userMgr), SessionId_(sessionId), Callback_(callback) {}
    void run() final { UserMgr_->enumerateUsersImpl(SessionId_, Callback_); }
  private:
    std::string SessionId_;
    Cb Callback_;
  };

  class UpdatePersonalFeeTask: public Task {
  public:
    UpdatePersonalFeeTask(UserManager *userMgr, const std::string &sessionId, Credentials &&credentials, DefaultCb callback) :
      Task(userMgr), SessionId_(sessionId), Credentials_(credentials), Callback_(callback) {}
    void run() final { UserMgr_->updatePersonalFeeImpl(SessionId_, Credentials_, Callback_); }

  private:
    std::string SessionId_;
    Credentials Credentials_;
    DefaultCb Callback_;
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
                  const std::string &poolHostProtocol,
                  const std::string &poolHostAddress,
                  const std::string &userActivateLinkPrefix,
                  const std::string &userChangePasswordLinkPrefix) {
    BaseCfg.PoolName = poolName;
    BaseCfg.PoolHostProtocol = poolHostProtocol;
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
      adminRecord.RegistrationDate = 0;
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

  coroutineTy *newCoroutine(coroutineProcTy entry, void *arg, unsigned stackSize) {
    CoroutineCounter_ += 1;
    return coroutineNewWithCb(entry, arg, stackSize, [](void *arg) {
      static_cast<UserManager*>(arg)->CoroutineCounter_ -= 1;
    }, this);
  }

  // Personal fee api
  intrusive_ptr<PersonalFeeTree> personalFeeConfig() {
    return intrusive_ptr<PersonalFeeTree>(PersonalFeeConfig_);
  }

  // Asynchronous api
  void userAction(const std::string &id, Task::DefaultCb callback) { startAsyncTask(new UserActionTask(this, uint512S(id), callback)); }
  void userActionInitiate(const std::string &login, UserActionRecord::EType type, Task::DefaultCb callback) { startAsyncTask(new UserInitiateActionTask(this, login, type, callback)); }
  void userChangePassword(const std::string &id, const std::string &newPassword, Task::DefaultCb callback) { startAsyncTask(new UserChangePasswordTask(this, uint512S(id), newPassword, callback)); }
  void userChangePasswordForce(const std::string &sessionId, const std::string &login, const std::string &newPassword, Task::DefaultCb callback) { startAsyncTask(new UserChangePasswordForceTask(this, sessionId, login, newPassword, callback)); }
  void userCreate(const std::string &login, Credentials &&credentials, Task::DefaultCb callback) { startAsyncTask(new UserCreateTask(this, login, std::move(credentials), callback)); }
  void userResendEmail(Credentials &&credentials, Task::DefaultCb callback) { startAsyncTask(new UserResendEmailTask(this, std::move(credentials), callback)); }
  void userLogin(Credentials &&credentials, UserLoginTask::Cb callback) { startAsyncTask(new UserLoginTask(this, std::move(credentials), callback)); }
  void userLogout(const std::string &id, Task::DefaultCb callback) { startAsyncTask(new UserLogoutTask(this, uint512S(id), callback)); }
  void updateCredentials(const std::string &id, const std::string &targetLogin, Credentials &&credentials, Task::DefaultCb callback) { startAsyncTask(new UpdateCredentialsTask(this, id, targetLogin, std::move(credentials), callback)); }
  void updateSettings(UserSettingsRecord &&settings, Task::DefaultCb callback) { startAsyncTask(new UpdateSettingsTask(this, std::move(settings), callback)); }
  void enumerateUsers(const std::string &sessionId, EnumerateUsersTask::Cb callback) { startAsyncTask(new EnumerateUsersTask(this, sessionId, callback)); }
  void updatePersonalFee(const std::string &sessionId, Credentials &&credentials, Task::DefaultCb callback) { startAsyncTask(new UpdatePersonalFeeTask(this, sessionId, std::move(credentials), callback)); }

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
  void userChangePasswordForceImpl(const std::string &sessionId, const std::string &login, const std::string &newPassword, Task::DefaultCb callback);
  void userCreateImpl(const std::string &login, Credentials &credentials, Task::DefaultCb callback);
  void resendEmailImpl(Credentials &credentials, Task::DefaultCb callback);
  void loginImpl(Credentials &credentials, UserLoginTask::Cb callback);
  void logoutImpl(const uint512 &sessionId, Task::DefaultCb callback);
  void updateCredentialsImpl(const std::string &sessionId, const std::string &targetLogin, const Credentials &credentials, Task::DefaultCb callback);
  void updateSettingsImpl(const UserSettingsRecord &settings, Task::DefaultCb callback);
  void enumerateUsersImpl(const std::string &sessionId, EnumerateUsersTask::Cb callback);
  void updatePersonalFeeImpl(const std::string &sessionId, const Credentials &credentials, Task::DefaultCb callback);

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

  bool updatePersonalFee(const std::string &login, const std::string &parentUserId, double defaultFee);

  void userManagerMain();
  void userManagerCleanup();

  asyncBase *Base_;
  aioUserEvent *TaskQueueEvent_;
  tbb::concurrent_queue<Task*> Tasks_;
  kvdb<rocksdbBase> UsersDb_;
  kvdb<rocksdbBase> UserPersonalFeeDb_;
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

  // User personal fees (RCU)
  atomic_intrusive_ptr<PersonalFeeTree> PersonalFeeConfig_;

  // Configuration
  std::vector<CCoinInfo> CoinInfo_;
  std::vector<BackendParameters> BackendParameters_;
  std::unordered_map<std::string, size_t> CoinIdxMap_;

  struct {
    std::string PoolName;
    std::string PoolHostProtocol;
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

  unsigned CoroutineCounter_ = 0;
};
