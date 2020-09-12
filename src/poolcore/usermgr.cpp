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

uint256 UserManager::generateHash(const std::string &login, const std::string &password)
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
    std::unique_ptr<rocksdbBase::IteratorType> It(UserSettingsDb_.iterator());
    for (It->seekFirst(); It->valid(); It->next()) {
      UserSettingsRecord settingsRecord;
      if (!settingsRecord.deserializeValue(It->value().data, It->value().size)) {
        LOG_F(ERROR, "Database corrupted (user settings)");
        exit(1);
      }

      std::string key = settingsRecord.Login;
      key.push_back('\0');
      key.append(settingsRecord.Coin);
      SettingsCache_.insert(std::make_pair(key, settingsRecord));
    }

    LOG_F(INFO, "UserManager: loaded %zu user settings records", SettingsCache_.size());
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
      LoginSessionMap_[sessionRecord.Login] = sessionRecord.Id;
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
      LoginActionMap_[actionRecord.Login] = actionRecord.Id;
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
        LoginSessionMap_.erase(session.second.Login);
        UserSessionsDb_.deleteRow(sessionBatch, session.second);
      } else if (session.second.Dirty) {
        UserSessionsDb_.put(sessionBatch, session.second);
        updatedSessions++;
      }
    }

    for (const auto &action: ActionsCache_) {
      if (currentTime - action.second.CreationDate >= ActionLifeTime_) {
        actionIdForDelete.push_back(action.second.Id);
        LoginActionMap_.erase(action.second.Login);
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
    callback("unknown_id");
    return;
  }

  UserActionRecord &actionRecord = It->second;
  UsersRecord userRecord;

  {
    decltype(UsersCache_)::const_accessor accessor;
    if (!UsersCache_.find(accessor, actionRecord.Login)) {
      callback("unknown_login");
      actionRemove(actionRecord);
      return;
    }

    userRecord = accessor->second;
  }

  bool userRecordUpdated = false;
  const char *status = "";
  switch (actionRecord.Type) {
    case UserActionRecord::UserActivate : {
      if (!userRecord.IsActive) {
        userRecord.IsActive = true;
        userRecordUpdated = true;
        status = "ok";
      } else {
        status = "user_already_active";
      }

      break;
    }
    default: {
      LOG_F(ERROR, "Action %s detected unknown type %u", id.ToString().c_str(), actionRecord.Type);
      status = "unknown_type";
      break;
    }
  }

  if (userRecordUpdated) {
    {
      decltype(UsersCache_)::accessor accessor;
      if (UsersCache_.find(accessor, actionRecord.Login))
        accessor->second = userRecord;
    }

    UsersDb_.put(userRecord);
  }

  actionRemove(actionRecord);
  callback(status);
}

void UserManager::userCreateImpl(Credentials &credentials, Task::DefaultCb callback)
{
  // NOTE: function is coroutine!

  // Check login format
  if (credentials.Login.empty() || credentials.Login.size() > 64 || credentials.Login == "admin") {
    callback("login_format_invalid");
    return;
  }

  // Check password format
  if (credentials.Password.size() < 8 || credentials.Password.size() > 64) {
    callback("password_format_invalid");
    return;
  }

  // Check email format
  if (credentials.EMail.size() > 256 || !validEmail(credentials.EMail)) {
    callback("email_format_invalid");
    return;
  }

  // Check name format
  if (credentials.Name.empty())
    credentials.Name = credentials.Login;
  if (credentials.Name.size() > 64) {
    callback("name_format_invalid");
    return;
  }

  // Check for known email
  if (AllEmails_.count(credentials.EMail)) {
    callback("duplicate_email");
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

  if (!UsersCache_.insert(std::pair(credentials.Login, userRecord))) {
    callback("duplicate_login");
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
      callback("smtp_client_create_error");
      return;
    }

    std::string EMailText;
    std::string activationLink = "http://";
      activationLink.append(BaseCfg.PoolHostAddress);
      activationLink.append(BaseCfg.ActivateLinkPrefix);
      activationLink.append(actionRecord.Id.ToString());

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
      callback("email_send_error");
      return;
    }

    smtpClientDelete(client);
  }

  // TODO: Setup default settings for all coins

  // Save changes to databases
  AllEmails_.insert(credentials.EMail);
  UsersDb_.put(userRecord);
  actionAdd(actionRecord);

  LOG_F(INFO, "New user: %s (%s) email: %s; actionId: %s", userRecord.Login.c_str(), userRecord.Name.c_str(), userRecord.EMail.c_str(), actionRecord.Id.ToString().c_str());
  callback("ok");
}

void UserManager::resendEmailImpl(Credentials &credentials, Task::DefaultCb callback)
{
  std::string email;
  {
    decltype (UsersCache_)::const_accessor accessor;
    if (!UsersCache_.find(accessor, credentials.Login)) {
      callback("invalid_password");
      return;
    }

    const UsersRecord &record = accessor->second;

    // Check password
    if (record.PasswordHash != generateHash(credentials.Login, credentials.Password)) {
      callback("invalid_password");
      return;
    }

    // Check activation
    if (record.IsActive) {
      callback("user_already_active");
      return;
    }

    email = record.EMail;
  }

  // Invalidate current action
  auto It = LoginActionMap_.find(credentials.Login);
  if (It != LoginActionMap_.end()) {
    UserActionRecord record;
    record.Id = It->second;
    record.Login = credentials.Login;
    actionRemove(record);
  }

  // Prepare new action
  UserActionRecord actionRecord;
  makeRandom(actionRecord.Id);
  actionRecord.Login = credentials.Login;
  actionRecord.Type = UserActionRecord::UserActivate;
  actionRecord.CreationDate = time(nullptr);

  // Send email
  if (SMTP.Enabled) {
    HostAddress localAddress;
    localAddress.ipv4 = INADDR_ANY;
    localAddress.family = AF_INET;
    localAddress.port = 0;
    SMTPClient *client = smtpClientNew(Base_, localAddress, SMTP.UseSmtps ? smtpServerSmtps : smtpServerPlain);
    if (!client) {
      LOG_F(ERROR, "Can't create smtp client");
      callback("smtp_client_create_error");
      return;
    }

    std::string EMailText;
    std::string activationLink = "http://";
      activationLink.append(BaseCfg.PoolHostAddress);
      activationLink.append(BaseCfg.ActivateLinkPrefix);
      activationLink.append(actionRecord.Id.ToString());

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
                                email.c_str(),
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
      callback("email_send_error");
      return;
    }

    smtpClientDelete(client);
  }

  actionAdd(actionRecord);
  LOG_F(INFO, "Resend email for %s to %s; actionId: %s", credentials.Login.c_str(), email.c_str(), actionRecord.Id.ToString().c_str());
  callback("ok");
}

void UserManager::loginImpl(Credentials &credentials, UserLoginTask::Cb callback)
{
  // Find user in db
  {
    decltype (UsersCache_)::const_accessor accessor;
    if (!UsersCache_.find(accessor, credentials.Login)) {
      callback("", "invalid_password");
      return;
    }

    const UsersRecord &record = accessor->second;

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
  }

  auto It = LoginSessionMap_.find(credentials.Login);
  if (It != LoginSessionMap_.end()) {
    callback(It->second.ToString(), "ok");
    return;
  }

  // Create new session
  UserSessionRecord session;
  makeRandom(session.Id);
  session.Login = credentials.Login;
  session.LastAccessTime = time(nullptr);
  sessionAdd(session);
  callback(session.Id.ToString(), "ok");
}

void UserManager::logoutImpl(const uint512 &sessionId, Task::DefaultCb callback)
{
  UserSessionRecord sessionRecord;
  {
    decltype (SessionsCache_)::const_accessor sessionAccessor;
    if (!SessionsCache_.find(sessionAccessor, sessionId)) {
      callback("unknown_id");
      return;
    }

    sessionRecord = sessionAccessor->second;
  }

  sessionRemove(sessionRecord);
  callback("ok");
}

void UserManager::updateSettingsImpl(const UserSettingsRecord &settings, Task::DefaultCb callback)
{
  std::string key = settings.Login;
  key.push_back('\0');
  key.append(settings.Coin);
  {
    decltype (SettingsCache_)::accessor accessor;
    if (SettingsCache_.find(accessor, key)) {
      accessor->second = settings;
    } else {
      SettingsCache_.insert(std::make_pair(key, settings));
    }
  }

  UserSettingsDb_.put(settings);
  callback("ok");
}

void UserManager::enumerateUsersImpl(EnumerateUsersTask::Cb callback)
{
  std::vector<Credentials> result;
  for (const auto &record: UsersCache_) {
    Credentials &credentials = result.emplace_back();
    credentials.Login = record.second.Login;
    credentials.Name = record.second.Name;
    credentials.EMail = record.second.EMail;
    credentials.RegistrationDate = record.second.RegistrationDate;
  }

  callback(result);
}

bool UserManager::checkUser(const std::string &login)
{
  return UsersCache_.count(login);
}

bool UserManager::checkPassword(const std::string &login, const std::string &password)
{
  decltype (UsersCache_)::const_accessor accessor;
  if (!UsersCache_.find(accessor, login))
    return false;

  const UsersRecord &record = accessor->second;

  // Check password
  return record.PasswordHash == generateHash(login, password);
}

bool UserManager::validateSession(const std::string &id, const std::string &targetLogin, std::string &resultLogin, bool needWriteAccess)
{
  time_t currentTime = time(nullptr);
  {
    decltype (SessionsCache_)::accessor accessor;
    if (SessionsCache_.find(accessor, uint512S(id))) {
      resultLogin = accessor->second.Login;
      accessor->second.updateLastAccessTime(currentTime);
    } else {
      return false;
    }
  }

  bool isSuperUser = (resultLogin == "admin") || (resultLogin == "observer" && !needWriteAccess);
  if (isSuperUser) {
    if (!targetLogin.empty()) {
      resultLogin = targetLogin;
      return UsersCache_.count(targetLogin);
    } else {
      return true;
    }
  } else {
    return targetLogin.empty();
  }
}

bool UserManager::getUserCredentials(const std::string &login, Credentials &out)
{
  decltype (UsersCache_)::const_accessor accessor;
  if (UsersCache_.find(accessor, login)) {
    out.Name = accessor->second.Name;
    out.EMail = accessor->second.EMail;
    out.RegistrationDate = accessor->second.RegistrationDate;
    return true;
  } else {
    return false;
  }
}

bool UserManager::getUserCoinSettings(const std::string &login, const std::string &coin, UserSettingsRecord &settings)
{
  std::string key = login;
  key.push_back('\0');
  key.append(coin);

  decltype (SettingsCache_)::const_accessor accessor;
  if (SettingsCache_.find(accessor, key)) {
    settings = accessor->second;
    return true;
  } else {
    return false;
  }
}
