#include "poolcore/usermgr.h"
#include "loguru.hpp"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <regex>
#include "asyncio/smtp.h"

static bool validEmail(const std::string &email)
{
  static const std::regex pattern("(\\w+)(\\.|_)?(\\w*)@(\\w+)(\\.(\\w+))+");
  return std::regex_match(email, pattern);
}

static uint256 generateHash(const std::string &login, const std::string &password)
{
  uint256 result;

  {
    // generate login hash (as a salt)
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, login.data(), login.size());
    SHA256_Final(result.begin(), &ctx);
  }
  {
    // sha256(password ++ loginHash)
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, password.data(), password.size());
    SHA256_Update(&ctx, result.begin(), sizeof(result));
    SHA256_Final(result.begin(), &ctx);
  }
  {
    // sha256 loop
    SHA256_CTX ctx;
    for (unsigned i = 0; i < 1499; i++) {
      SHA256_Init(&ctx);
      SHA256_Update(&ctx, result.begin(), sizeof(result));
      SHA256_Final(result.begin(), &ctx);
    }
  }

  return result;
}

template<unsigned BITS>
static void makeRandom(base_blob<BITS> &number)
{
  RAND_bytes(number.begin(), number.size());
}

UserManager::UserManager(const std::filesystem::path &dbPath) :
  UsersDb_(dbPath / "users"),
  UserSettingsDb_(dbPath / "usersettings"),
  UserActionsDb_(dbPath / "useractions"),
  UserSessionsDb_(dbPath / "usersessions")
{
  // Load all users data to memory
  {
    // Users
    std::unique_ptr<rocksdbBase::IteratorType> It(UsersDb_.iterator());
    for (It->seekFirst(); It->valid(); It->next()) {
      UsersRecord userRecord;
      if (!userRecord.deserializeValue(It->value().data, It->value().size)) {
        LOG_F(ERROR, "Database corrupted (users)");
        exit(1);
      }

      UsersCache_.insert(std::make_pair(userRecord.Login, userRecord));
      if (!AllEmails_.insert(userRecord.EMail).second) {
        LOG_F(ERROR, "Non-unique email detected: %s", userRecord.EMail.c_str());
        exit(1);
      }
    }

    LOG_F(INFO, "UserManager: loaded %zu user records", UsersCache_.size());
  }

  {
    // User settings
  }

  {
    // Sessions
    std::unique_ptr<rocksdbBase::IteratorType> It(UserSessionsDb_.iterator());
    for (It->seekFirst(); It->valid(); It->next()) {
      UserSessionRecord sessionRecord;
      if (!sessionRecord.deserializeValue(It->value().data, It->value().size)) {
        LOG_F(ERROR, "Database corrupted (user sessions) -> close all sessions");
        UserSessionsDb_.clear();
        SessionsCache_.clear();
        break;
      }

      SessionsCache_.insert(std::make_pair(sessionRecord.Id, sessionRecord));
    }

    LOG_F(INFO, "UserManager: loaded %zu user sessions", SessionsCache_.size());
  }

  {
    // Actions
    std::unique_ptr<rocksdbBase::IteratorType> It(UserActionsDb_.iterator());
    for (It->seekFirst(); It->valid(); It->next()) {
      UserActionRecord actionRecord;
      if (!actionRecord.deserializeValue(It->value().data, It->value().size)) {
        LOG_F(ERROR, "Database corrupted (user actions) -> cancel all actions");
        It->cleanup();
        UserActionsDb_.clear();
        ActionsCache_.clear();
        break;
      }

      ActionsCache_.insert(std::make_pair(actionRecord.Id, actionRecord));
    }

    LOG_F(INFO, "UserManager: loaded %zu user actions", ActionsCache_.size());
  }


  Base_ = createAsyncBase(amOSDefault);
  TaskQueueEvent_ = newUserEvent(Base_, 0, [](aioUserEvent*, void *arg) {
    Task *task = nullptr;
    UserManager *userMgr = static_cast<UserManager*>(arg);
    while (userMgr->Tasks_.try_pop(task))
      task->run();
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

  // Run cleanup coroutine
  CleanupEvent_ = newUserEvent(Base_, 0, 0, 0);
  coroutineTy *cleanupCoro = coroutineNew([](void *arg) {
    static_cast<UserManager*>(arg)->userManagerCleanup();
  }, this, 0x10000);

  coroutineCall(cleanupCoro);
  asyncLoop(Base_);
}

void UserManager::userManagerCleanup()
{
  for (;;) {
    size_t usersDeletedCount = 0;
    size_t updatedSessions = 0;
    time_t currentTime = time(nullptr);
    std::vector<uint512> sessionIdForDelete;
    std::vector<uint512> actionIdForDelete;
    rocksdbBase::PartitionBatchType sessionBatch = UserSessionsDb_.batch("default");
    rocksdbBase::PartitionBatchType actionBatch = UserActionsDb_.batch("default");

    for (const auto &session: SessionsCache_) {
      if (currentTime - session.second.LastAccessTime >= SessionLifeTime_) {
        sessionIdForDelete.push_back(session.second.Id);
        UserSessionsDb_.deleteRow(sessionBatch, session.second);
      } else if (session.second.Dirty) {
        UserSessionsDb_.put(sessionBatch, session.second);
        updatedSessions++;
      }
    }

    for (const auto &action: ActionsCache_) {
      if (currentTime - action.second.CreationDate >= ActionLifeTime_) {
        actionIdForDelete.push_back(action.second.Id);
        UserActionsDb_.deleteRow(actionBatch, action.second);
        if (action.second.Type == UserActionRecord::UserActivate) {
          // Delete not activated user record
          tbb::concurrent_hash_map<std::string, UsersRecord>::accessor accessor;
          if (UsersCache_.find(accessor, action.second.Login)) {
            AllEmails_.erase(accessor->second.EMail);
            UsersDb_.deleteRow(accessor->second);
            UsersCache_.erase(accessor);
            usersDeletedCount++;
          }
        }
      }
    }

    // Apply changes
    for (const auto &sessionId: sessionIdForDelete)
      SessionsCache_.erase(sessionId);
    for (const auto &actionId: actionIdForDelete)
      ActionsCache_.erase(actionId);
    UserSessionsDb_.writeBatch(sessionBatch);
    UserActionsDb_.writeBatch(actionBatch);
    LOG_F(INFO, "(CLEANER) Removed sessions: %zu; actions: %zu; inactive users: %zu; updated sessions: %zu", sessionIdForDelete.size(), actionIdForDelete.size(), usersDeletedCount, updatedSessions);
    ioSleep(CleanupEvent_, CleanupInterval_ * 1000000);
  }
}

void UserManager::startAsyncTask(Task *task)
{
  Tasks_.push(task);
  userEventActivate(TaskQueueEvent_);
}

void UserManager::actionImpl(const uint512 &id, Task::DefaultCb callback)
{
  auto It = ActionsCache_.find(id);
  if (It == ActionsCache_.end()) {
    callback(false, "unknown_id");
    return;
  }

  UserActionRecord &actionRecord = It->second;
  switch (actionRecord.Type) {
    case UserActionRecord::UserActivate : {
      // Check user status
      decltype(UsersCache_)::accessor accessor;
      if (!UsersCache_.find(accessor, actionRecord.Login)) {
        LOG_F(ERROR, "Action %s linked to non existent login %s", id.ToString().c_str(), actionRecord.Login.c_str());
        callback(false, "unknown_login");
        return;
      }

      UsersRecord &userRecord = accessor->second;
      if (userRecord.IsActive) {
        LOG_F(ERROR, "User %s already active", id.ToString().c_str());
        callback(false, "user_already_active");
        return;
      }

      // Activate user
      userRecord.IsActive = true;
      actionRemove(actionRecord, userRecord);
      callback(true, "ok");
      return;
    }
    default: {
      LOG_F(ERROR, "Action %s detected unknown type %u", id.ToString().c_str(), actionRecord.Type);
      callback(false, "unknown_type");
      return;
    }
  }
}

void UserManager::userCreateImpl(Credentials &credentials, Task::DefaultCb callback)
{
  // NOTE: function is coroutine!

  // Check login format
  if (credentials.Login.empty() || credentials.Login.size() > 64 || credentials.Login == "admin") {
    callback(false, "login_format_invalid");
    return;
  }

  // Check password format
  if (credentials.Password.size() < 8 || credentials.Password.size() > 64) {
    callback(false, "password_format_invalid");
    return;
  }

  // Check email format
  if (credentials.EMail.size() > 256 || !validEmail(credentials.EMail)) {
    callback(false, "email_format_invalid");
    return;
  }

  // Check name format
  if (credentials.Name.empty())
    credentials.Name = credentials.Login;
  if (credentials.Name.size() > 64) {
    callback(false, "name_format_invalid");
    return;
  }

  // Check for known email
  if (AllEmails_.count(credentials.EMail)) {
    callback(false, "duplicate_email");
    return;
  }

  // Prepare credentials (hashing password, activate link, etc...)
  // Generate 'user activate' action
  // Insert data into memory storage
  UserActionRecord actionRecord;
  makeRandom(actionRecord.Id);
  actionRecord.Login = credentials.Login;
  actionRecord.Type = UserActionRecord::UserActivate;
  actionRecord.CreationDate = time(nullptr);

  UsersRecord userRecord;
  userRecord.Login = credentials.Login;
  userRecord.EMail = credentials.EMail;
  userRecord.Name = credentials.Name;
  userRecord.TwoFactorAuthData.clear();
  userRecord.PasswordHash = generateHash(credentials.Login, credentials.Password);
  userRecord.RegistrationDate = time(nullptr);
  userRecord.IsActive = false;
  userRecord.CurrentSessionId.SetNull();
  userRecord.CurrentActionId = actionRecord.Id;

  if (!UsersCache_.insert(std::pair(credentials.Login, userRecord))) {
    callback(false, "duplicate_login");
    return;
  }

  if (SMTP.Enabled) {
    HostAddress localAddress;
    localAddress.ipv4 = INADDR_ANY;
    localAddress.family = AF_INET;
    localAddress.port = 0;
    SMTPClient *client = smtpClientNew(Base_, localAddress, SMTP.UseSmtps ? smtpServerSmtps : smtpServerPlain);
    if (!client) {
      LOG_F(ERROR, "Can't create smtp client");
      callback(false, "smtp_client_create_error");
      return;
    }

    std::string EMailText;
    std::string activationLink = "http://";
      activationLink.append(BaseCfg.PoolHostAddress);
      activationLink.append(BaseCfg.ActivateLinkPrefix);
      activationLink.append(userRecord.CurrentActionId.ToString());

    EMailText.append("Content-Type: text/html; charset=\"ISO-8859-1\";\r\n");
    EMailText.append("This email generated automatically, please don't reply.\r\n");
    EMailText.append("For finish registration visit <a href=\"");
    EMailText.append(activationLink);
    EMailText.append("\">");
    EMailText.append(activationLink);
    EMailText.append("</a>\r\n");

    int result = ioSmtpSendMail(client,
                                SMTP.ServerAddress,
                                SMTP.UseStartTls,
                                BaseCfg.PoolHostAddress.c_str(),
                                SMTP.Login.c_str(),
                                SMTP.Password.c_str(),
                                SMTP.SenderAddress.c_str(),
                                userRecord.EMail.c_str(),
                                ("Registration at " + BaseCfg.PoolName).c_str(),
                                EMailText.c_str(),
                                afNone,
                                16000000);
    if (result != 0) {
      if (result == -smtpError)
        LOG_F(ERROR, "SMTP error; code: %u; text: %s", smtpClientGetResultCode(client), smtpClientGetResponse(client));
      else
        LOG_F(ERROR, "SMTP client error %u", -result);
      smtpClientDelete(client);
      callback(false, "email_send_error");
      return;
    }

    smtpClientDelete(client);
  }

  // Modify memory databases
  AllEmails_.insert(credentials.EMail);
  ActionsCache_.insert(std::make_pair(actionRecord.Id, actionRecord));

  // Modify disk databases
  UsersDb_.put(userRecord);
  UserActionsDb_.put(actionRecord);

  LOG_F(INFO, "New user: %s (%s) email: %s; actionId: %s", userRecord.Login.c_str(), userRecord.Name.c_str(), userRecord.EMail.c_str(), userRecord.CurrentActionId.ToString().c_str());
  callback(true, "ok");
}

void UserManager::loginImpl(Credentials &credentials, UserLoginTask::Cb callback)
{
  // Find user in db
  decltype (UsersCache_)::accessor accessor;
  if (!UsersCache_.find(accessor, credentials.Login)) {
    callback("", "unknown_login");
    return;
  }

  UsersRecord &record = accessor->second;

  // Check password
  if (record.PasswordHash != generateHash(credentials.Login, credentials.Password)) {
    callback("", "invalid_password");
    return;
  }

  // Check activation
  if (!record.IsActive) {
    callback("", "user_not_active");
    return;
  }

  // Check existing session
  if (!record.CurrentSessionId.IsNull()) {
    callback(record.CurrentSessionId.ToString(), "ok");
    return;
  }

  // Create new session
  UserSessionRecord session;
  makeRandom(session.Id);
  session.Login = record.Login;
  session.LastAccessTime = time(nullptr);
  sessionAdd(session, record);
  callback(session.Id.ToString(), "ok");
}

void UserManager::logoutImpl(const uint512 &sessionId, Task::DefaultCb callback)
{
  UserSessionRecord sessionRecord;
  {
    decltype (SessionsCache_)::const_accessor sessionAccessor;
    if (!SessionsCache_.find(sessionAccessor, sessionId)) {
      callback(false, "unknown_id");
      return;
    }

    sessionRecord = sessionAccessor->second;
  }

  decltype (UsersCache_)::accessor usersAccessor;
  if (!UsersCache_.find(usersAccessor, sessionRecord.Login)) {
    LOG_F(ERROR, "Session %s refers to non existent login %s", sessionId.ToString().c_str(), sessionRecord.Login.c_str());
    callback(false, "unknown_login");
    return;
  }

  sessionRemove(sessionRecord, usersAccessor->second);
  callback(true, "ok");
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
