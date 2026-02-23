#include "poolcore/usermgr.h"
#include "poolcore/backend.h"
#include "poolcommon/totp.h"
#include "blockmaker/sha256.h"
#include "loguru.hpp"
#include <openssl/rand.h>
#include <regex>
#include "asyncio/smtp.h"

static bool validEmail(const std::string &email)
{
  static const std::regex pattern("(\\w+)(\\.|_)?(\\w*)@(\\w+)(\\.(\\w+))+");
  return std::regex_match(email, pattern);
}

BaseBlob<256> UserManager::generateHash(const std::string &login, const std::string &password)
{
  BaseBlob<256> result;

  {
    // generate login hash (as a salt)
    CCtxSha256 ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, login.data(), login.size());
    sha256Final(&ctx, result.begin());
  }
  {
    // sha256(password ++ loginHash)
    CCtxSha256 ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, password.data(), password.size());
    sha256Update(&ctx, result.begin(), sizeof(result));
    sha256Final(&ctx, result.begin());
  }
  {
    // sha256 loop
    CCtxSha256 ctx;
    for (unsigned i = 0; i < 1499; i++) {
      sha256Init(&ctx);
      sha256Update(&ctx, result.begin(), sizeof(result));
      sha256Final(&ctx, result.begin());
    }
  }

  return result;
}

bool UserManager::sendMail(const std::string &login, const std::string &emailAddress, const std::string &emailTitlePrefix, const std::string &linkPrefix, const BaseBlob<512> &actionId, const std::string &mainText, std::string &error)
{
  HostAddress localAddress;
  localAddress.ipv4 = INADDR_ANY;
  localAddress.family = AF_INET;
  localAddress.port = 0;
  SMTPClient *client = smtpClientNew(Base_, localAddress, SMTP.UseSmtps ? smtpServerSmtps : smtpServerPlain);
  if (!client) {
    LOG_F(ERROR, "Can't create smtp client");
    error = "smtp_client_create_error";
    return false;
  }

  std::string EMailText;
  std::string activationLink = BaseCfg.PoolHostProtocol + "://";
    activationLink.append(BaseCfg.PoolHostAddress);
    activationLink.append(linkPrefix);
    activationLink.append(actionId.getHexLE());

  EMailText.append("Content-Type: text/html; charset=\"ISO-8859-1\";\r\n");
  EMailText.append("This email generated automatically, please don't reply.\r\n");
  EMailText.append(mainText);
  EMailText.append(" visit <a href=\"");
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
                              emailAddress.c_str(),
                              (emailTitlePrefix + BaseCfg.PoolName).c_str(),
                              EMailText.c_str(),
                              afNone,
                              16000000);
  if (result != 0) {
    if (result == -smtpError)
      LOG_F(ERROR, "SMTP error; code: %u; text: %s", smtpClientGetResultCode(client), smtpClientGetResponse(client));
    else
      LOG_F(ERROR, "SMTP client error %u", -result);
    smtpClientDelete(client);
    error = "email_send_error";
    return false;
  }


  smtpClientDelete(client);
  LOG_F(INFO, "%s %s: %s", login.c_str(), emailTitlePrefix.c_str(), actionId.getHexLE().c_str());
  return true;
}

bool UserManager::check2fa(const std::string &secret, const std::string &receivedCode)
{
  if (!secret.empty()) {
    if (receivedCode.size() != 6)
      return false;

    int receivedCodeAsInt = atoi(receivedCode.c_str());
    unsigned long currentTime = time(nullptr) / 30;
    int prev = generateCode(secret.c_str(), currentTime-1);
    int current = generateCode(secret.c_str(), currentTime);
    int next = generateCode(secret.c_str(), currentTime+1);
    if (receivedCodeAsInt != prev && receivedCodeAsInt != current && receivedCodeAsInt != next)
      return false;
  }

  return true;
}

template<unsigned BITS>
static void makeRandom(BaseBlob<BITS> &number)
{
  RAND_bytes(number.begin(), number.size());
}

UserManager::UserManager(const std::filesystem::path &dbPath) :
  UsersDb_(dbPath / "users"),
  UserFeePlanDb_(dbPath / "userfeeplan"),
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

      // Ignore special users
      if (userRecord.Name == "admin" || userRecord.Name == "observer")
        continue;

      UsersCache_.insert(std::make_pair(userRecord.Login, userRecord));
      if (!userRecord.EMail.empty()) {
        if (!AllEmails_.insert(userRecord.EMail).second) {
          LOG_F(ERROR, "Non-unique email detected: %s", userRecord.EMail.c_str());
          exit(1);
        }
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
      if (!sessionRecord.IsPermanent)
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

  {
    // Fee plan
    FeePlan defaultPlan;
    defaultPlan.Modes.resize(static_cast<size_t>(EMiningMode::Count));
    FeePlanCache_.insert(std::make_pair("default", std::move(defaultPlan)));
    std::unique_ptr<rocksdbBase::IteratorType> It(UserFeePlanDb_.iterator());
    for (It->seekFirst(); It->valid(); It->next()) {
      UserFeePlanRecord record;
      if (!record.deserializeValue(It->value().data, It->value().size)) {
        LOG_F(ERROR, "Database corrupted (fee plan)");
        break;
      }

      std::string error;
      if (!acceptFeePlanRecord(record, error))
        break;
    }
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
  // Wait all coroutines
  if (CoroutineCounter_) {
    LOG_F(INFO, "Wait %u coroutines", CoroutineCounter_);
    while (CoroutineCounter_)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  postQuitOperation(Base_);
  Thread_.join();
}

bool UserManager::acceptFeePlanRecord(const UserFeePlanRecord &record, std::string &error)
{
  // Validate individual entries and compute sum; extraFee is added to the total (PPS pool fee)
  auto acceptProc = [this](
    const std::vector<UserFeePair> &config, const std::string &planId, double extraFee, std::string &error) -> bool
  {
    double sum = extraFee;
    for (const auto &pair: config) {
      if (UsersCache_.count(pair.UserId) == 0) {
        error = "unknown_login";
        LOG_F(ERROR, "UserManager: can't accept fee plan '%s', user %s not exists", planId.c_str(), pair.UserId.c_str());
        return false;
      }

      if (pair.Percentage < 0.0) {
        error = "invalid_percentage";
        LOG_F(ERROR, "UserManager: can't accept fee plan '%s', negative percentage for user %s", planId.c_str(), pair.UserId.c_str());
        return false;
      }

      sum += pair.Percentage;
    }

    if (sum >= 100.0) {
      error = "fee_too_much";
      LOG_F(ERROR, "UserManager: can't accept fee plan '%s', fee %.2lf too big", planId.c_str(), sum);
      return false;
    }

    return true;
  };

  auto feeConfigToString = [](const std::vector<UserFeePair> &config) -> std::string {
    std::string result;
    for (const auto &pair: config) {
      result.append(pair.UserId);
      result.push_back('(');
      result.append(std::to_string(pair.Percentage));
      result.append("%) ");
    }

    return result;
  };

  for (size_t i = 0, ie = record.Modes.size(); i != ie; ++i) {
    EMiningMode mode = static_cast<EMiningMode>(i);
    const auto &modeCfg = record.Modes[i];

    if (mode == EMiningMode::PPS) {
      // Find max PPS pool fee among coins not covered by coin-specific config
      std::unordered_set<std::string> coveredCoins;
      for (const auto &coinCfg : modeCfg.CoinSpecific)
        coveredCoins.insert(coinCfg.CoinName);

      double maxUncoveredPpsFee = 0.0;
      for (const auto &[coinName, backend] : Backends_) {
        if (!coveredCoins.count(coinName))
          maxUncoveredPpsFee = std::max(
            maxUncoveredPpsFee, backend->accountingDb()->backendSettings().PPSConfig.PoolFee);
      }

      // Validate default with worst-case PPS pool fee
      if (!acceptProc(modeCfg.Default, record.FeePlanId, maxUncoveredPpsFee, error))
        return false;

      // Validate coin-specific entries with that coin's PPS pool fee
      for (const auto &coinCfg : modeCfg.CoinSpecific) {
        double extraFee = 0.0;
        auto backendIt = Backends_.find(coinCfg.CoinName);
        if (backendIt != Backends_.end())
          extraFee = backendIt->second->accountingDb()->backendSettings().PPSConfig.PoolFee;
        if (!acceptProc(coinCfg.Config, record.FeePlanId, extraFee, error))
          return false;
      }
    } else {
      // PPLNS: no extra fee
      if (!acceptProc(modeCfg.Default, record.FeePlanId, 0.0, error))
        return false;
      for (const auto &coinCfg : modeCfg.CoinSpecific) {
        if (!acceptProc(coinCfg.Config, record.FeePlanId, 0.0, error))
          return false;
      }
    }
  }

  FeePlan plan;
  plan.Modes.resize(record.Modes.size());
  for (size_t i = 0, ie = record.Modes.size(); i != ie; ++i) {
    plan.Modes[i].Default = record.Modes[i].Default;
    for (const auto &coinCfg: record.Modes[i].CoinSpecific)
      plan.Modes[i].CoinSpecific[coinCfg.CoinName] = coinCfg.Config;
  }
  plan.ReferralId = record.ReferralId;

  FeePlanCache_.erase(record.FeePlanId);
  FeePlanCache_.insert(std::make_pair(record.FeePlanId, plan));

  if (!record.ReferralId.isNull())
    ReferralIdMap_[record.ReferralId] = record.FeePlanId;

  LOG_F(INFO, "UserManager: accepted fee plan %s", record.FeePlanId.c_str());
  for (size_t i = 0, ie = record.Modes.size(); i != ie; ++i) {
    const char *modeName = miningModeName(static_cast<EMiningMode>(i));
    LOG_F(INFO, " [%s] default: %s", modeName, feeConfigToString(record.Modes[i].Default).c_str());
    for (const auto &coinCfg: record.Modes[i].CoinSpecific)
      LOG_F(INFO, " [%s] %s: %s", modeName, coinCfg.CoinName.c_str(), feeConfigToString(coinCfg.Config).c_str());
  }

  return true;
}

void UserManager::buildModeFeeConfig(const FeePlan::ModeConfig &mode, CModeFeeConfig &result)
{
  auto sortByUserId = [](const UserFeePair &l, const UserFeePair &r) { return l.UserId < r.UserId; };

  result.Default = mode.Default;
  std::sort(result.Default.begin(), result.Default.end(), sortByUserId);

  result.CoinSpecific.clear();
  for (const auto &coinFee: mode.CoinSpecific) {
    auto &cfg = result.CoinSpecific.emplace_back();
    cfg.CoinName = coinFee.first;
    cfg.Config = coinFee.second;
    std::sort(cfg.Config.begin(), cfg.Config.end(), sortByUserId);
  }

  std::sort(
    result.CoinSpecific.begin(),
    result.CoinSpecific.end(),
    [](const CUserFeeConfig &l, const CUserFeeConfig &r) { return l.CoinName < r.CoinName; });
}

void UserManager::buildFeePlanRecord(const std::string &feePlanId, const FeePlan &plan, UserFeePlanRecord &result)
{
  result.FeePlanId = feePlanId;
  result.Modes.resize(plan.Modes.size());
  for (size_t i = 0, ie = plan.Modes.size(); i != ie; ++i)
    buildModeFeeConfig(plan.Modes[i], result.Modes[i]);
  result.ReferralId = plan.ReferralId;
}

void UserManager::collectLinkedFeePlans(const std::string &userId, std::unordered_set<std::string> &plans)
{
  for (const auto &entry: FeePlanCache_) {
    for (const auto &modeCfg: entry.second.Modes) {
      for (const auto &pair: modeCfg.Default) {
        if (pair.UserId == userId)
          plans.insert(entry.first);
      }
      for (const auto &coin: modeCfg.CoinSpecific) {
        for (const auto &pair: coin.second) {
          if (pair.UserId == userId)
            plans.insert(entry.first);
        }
      }
    }
  }
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
    std::vector<BaseBlob<512>> sessionIdForDelete;
    std::vector<BaseBlob<512>> actionIdForDelete;
    kvdb<rocksdbBase>::Batch sessionBatch;
    kvdb<rocksdbBase>::Batch actionBatch;

    for (const auto &session: SessionsCache_) {
      if (!session.second.IsPermanent && (currentTime - session.second.LastAccessTime >= SessionLifeTime_)) {
        sessionIdForDelete.push_back(session.second.Id);
        LoginSessionMap_.erase(session.second.Login);
        sessionBatch.deleteRow(session.second);
      } else if (session.second.Dirty) {
        sessionBatch.put(session.second);
        updatedSessions++;
      }
    }

    for (const auto &action: ActionsCache_) {
      if (currentTime - action.second.CreationDate >= ActionLifeTime_) {
        actionIdForDelete.push_back(action.second.Id);
        LoginActionMap_.erase(action.second.Login);
        actionBatch.deleteRow(action.second);
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

void UserManager::actionImpl(const BaseBlob<512> &id, const std::string &newPassword, const std::string &totp, Task::DefaultCb callback)
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

  // Don't allow special users
  if (actionRecord.Login == "admin" || actionRecord.Login == "observer") {
    callback("unknown_login");
    return;
  }

  bool actionNeedRemove = true;
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

    case UserActionRecord::UserChangePassword : {
      // Check password format
      if (newPassword.size() < 8 || newPassword.size() > 64) {
        status = "password_format_invalid";
        break;
      }

      userRecord.PasswordHash = generateHash(actionRecord.Login, newPassword);
      userRecordUpdated = true;
      status = "ok";
      break;
    }

    case UserActionRecord::UserTwoFactorActivate : {
      if (userRecord.TwoFactorAuthData.empty()) {
        if (check2fa(actionRecord.TwoFactorKey, totp)) {
          userRecord.TwoFactorAuthData = actionRecord.TwoFactorKey;
          userRecordUpdated = true;
          status = "ok";
        } else {
          status = "2fa_invalid";
          actionNeedRemove = false;
        }
      } else {
        status = "2fa_already_activated";
      }

      break;
    }

    case UserActionRecord::UserTwoFactorDeactivate : {
      if (!userRecord.TwoFactorAuthData.empty()) {
        userRecord.TwoFactorAuthData.clear();
        userRecordUpdated = true;
        status = "ok";
      } else {
        status = "2fa_not_activated";
      }

      break;
    }

    default: {
      LOG_F(ERROR, "Action %s detected unknown type %u", id.getHexLE().c_str(), actionRecord.Type);
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

  if (actionNeedRemove)
    actionRemove(actionRecord);
  callback(status);
}

void UserManager::changePasswordInitiateImpl(const std::string &login, Task::DefaultCb callback)
{
  // Don't allow special users
  if (login == "admin" || login == "observer") {
    callback("unknown_login");
    return;
  }

  std::string emailAddress;
  decltype (UsersCache_)::const_accessor accessor;
  if (UsersCache_.find(accessor, login)) {
    emailAddress = accessor->second.EMail;
  } else {
    callback("unknown_login");
    return;
  }

  UserActionRecord actionRecord;
  makeRandom(actionRecord.Id);
  actionRecord.Login = login;
  actionRecord.Type = UserActionRecord::UserChangePassword;
  actionRecord.CreationDate = time(nullptr);

  // Send email
  std::string emailSendError;
  if (SMTP.Enabled && !sendMail(login, emailAddress, "Change password at ", BaseCfg.ChangePasswordLinkPrefix, actionRecord.Id, "For change your password", emailSendError)) {
    callback(emailSendError.c_str());
    return;
  }

  actionAdd(actionRecord);
  callback("ok");
}

void UserManager::userChangePasswordForceImpl(const std::string &sessionId, const std::string &login, const std::string &newPassword, Task::DefaultCb callback)
{
  UserWithAccessRights tokenInfo;
  if (!validateSession(sessionId, "admin", tokenInfo, true) || tokenInfo.Login != "admin") {
    callback("unknown_id");
    return;
  }

  // Check password format
  if (newPassword.size() < 8 || newPassword.size() > 64) {
    callback("password_format_invalid");
    return;
  }

  UsersRecord userRecord;

  {
    decltype(UsersCache_)::const_accessor accessor;
    if (!UsersCache_.find(accessor, login)) {
      callback("unknown_login");
      return;
    }

    userRecord = accessor->second;
  }

  userRecord.PasswordHash = generateHash(login, newPassword);
  {
    decltype(UsersCache_)::accessor accessor;
    if (UsersCache_.find(accessor, login))
      accessor->second = userRecord;
  }

  UsersDb_.put(userRecord);
  callback("ok");
}

void UserManager::userCreateImpl(const std::string &login, Credentials &credentials, Task::DefaultCb callback)
{
  // NOTE: function is coroutine!

  // Check login format
  if (credentials.Login.empty() || credentials.Login.size() > 64 || credentials.Login == "admin" || credentials.Login == "observer") {
    callback("login_format_invalid");
    return;
  }

  for (char c : credentials.Login) {
    if (c < 32 || c > 126) {
      callback("login_format_invalid");
      return;
    }
  }

  // Check password format
  if (credentials.Password.size() < 8 || credentials.Password.size() > 64) {
    callback("password_format_invalid");
    return;
  }

  // Check email format
  if (!credentials.EMail.empty()) {
    if (credentials.EMail.size() > 256 || !validEmail(credentials.EMail)) {
      callback("email_format_invalid");
      return;
    }
  } else if (!credentials.IsActive) {
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
  if (!credentials.EMail.empty() && AllEmails_.count(credentials.EMail)) {
    callback("duplicate_email");
    return;
  }

  // Check special flags available for admin only
  if (credentials.IsActive || credentials.IsReadOnly) {
    if (login != "admin") {
      callback("unknown_id");
      return;
    }
  }

  // Check personal fee plan
  std::string feePlan = credentials.FeePlan.empty() ? "default" : credentials.FeePlan;
  if (feePlan != "default") {
    // Direct assignment by name: admin only
    if (login != "admin") {
      callback("fee_plan_not_allowed");
      return;
    }

    if (!FeePlanCache_.count(feePlan)) {
      callback("fee_plan_not_exists");
      return;
    }
  } else if (!credentials.ReferralId.empty()) {
    // Referral registration (only when feePlanId not set explicitly)
    auto referralId = BaseBlob<256>::fromHexRaw(credentials.ReferralId.c_str());
    auto it = ReferralIdMap_.find(referralId);
    if (it == ReferralIdMap_.end()) {
      callback("invalid_referral_id");
      return;
    }
    feePlan = it->second;
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
  userRecord.IsActive = credentials.IsActive;
  userRecord.IsReadOnly = credentials.IsReadOnly;
  userRecord.FeePlanId = feePlan;

  if (!UsersCache_.insert(std::pair(credentials.Login, userRecord))) {
    callback("duplicate_login");
    return;
  }

  if (!credentials.IsActive) {
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
      std::string activationLink = BaseCfg.PoolHostProtocol + "://";
        activationLink.append(BaseCfg.PoolHostAddress);
        activationLink.append(BaseCfg.ActivateLinkPrefix);
        activationLink.append(actionRecord.Id.getHexLE());

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
  }

  if (!credentials.IsActive) {
    // Save changes to databases
    actionAdd(actionRecord);
  }

  if (!credentials.EMail.empty())
    AllEmails_.insert(credentials.EMail);

  UsersDb_.put(userRecord);

  // Notify all backends about new user's fee plan
  for (const auto &[coinName, backend] : Backends_)
    backend->sendUserFeePlanChange(credentials.Login, feePlan);

  LOG_F(INFO, "New user: %s (%s) email: %s; actionId: %s", userRecord.Login.c_str(), userRecord.Name.c_str(), userRecord.EMail.c_str(), actionRecord.Id.getHexLE().c_str());
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
    std::string activationLink = BaseCfg.PoolHostProtocol + "://";
      activationLink.append(BaseCfg.PoolHostAddress);
      activationLink.append(BaseCfg.ActivateLinkPrefix);
      activationLink.append(actionRecord.Id.getHexLE());

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
  LOG_F(INFO, "Resend email for %s to %s; actionId: %s", credentials.Login.c_str(), email.c_str(), actionRecord.Id.getHexLE().c_str());
  callback("ok");
}

void UserManager::loginImpl(Credentials &credentials, UserLoginTask::Cb callback)
{
  // Find user in db
  bool isReadOnly = false;

  {
    decltype (UsersCache_)::const_accessor accessor;
    if (!UsersCache_.find(accessor, credentials.Login)) {
      callback("", "invalid_password", false);
      return;
    }

    const UsersRecord &record = accessor->second;

    // Check password
    if (record.PasswordHash != generateHash(credentials.Login, credentials.Password)) {
      callback("", "invalid_password", false);
      return;
    }

    // Check 2fa
    if (!check2fa(record.TwoFactorAuthData, credentials.TwoFactor)) {
      callback("", "2fa_invalid", false);
      return;
    }

    // Check activation
    if (!record.IsActive) {
      callback("", "user_not_active", false);
      return;
    }

    isReadOnly = record.IsReadOnly;
  }

  auto It = LoginSessionMap_.find(credentials.Login);
  if (It != LoginSessionMap_.end()) {
    callback(It->second.getHexLE(), "ok", isReadOnly);
    return;
  }

  // Create new session
  UserSessionRecord session;
  makeRandom(session.Id);
  session.Login = credentials.Login;
  session.LastAccessTime = time(nullptr);
  session.IsReadOnly = isReadOnly;
  sessionAdd(session);
  callback(session.Id.getHexLE(), "ok", isReadOnly);
}

void UserManager::logoutImpl(const BaseBlob<512> &sessionId, Task::DefaultCb callback)
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

  if (!sessionRecord.IsPermanent)
    sessionRemove(sessionRecord);
  callback("ok");
}

void UserManager::queryMonitoringSessionImpl(const std::string &sessionId, const std::string &targetLogin, UserQueryMonitoringSessionTask::Cb callback)
{
  UserWithAccessRights tokenInfo;
  if (!validateSession(sessionId, targetLogin, tokenInfo, false)) {
    callback("", "unknown_id");
    return;
  }

  // Check user structure for existing session id
  {
    decltype (UsersCache_)::accessor accessor;
    if (!UsersCache_.find(accessor, tokenInfo.Login)) {
      callback("", "unknown_login");
      return;
    }

    if (!accessor->second.MonitoringSessionId.empty()) {
      callback(accessor->second.MonitoringSessionId, "ok");
      return;
    }
  }

  // We don't have monitoring session at this moment, need to create it
  UserSessionRecord session;
  makeRandom(session.Id);
  session.Login = tokenInfo.Login;
  session.LastAccessTime = time(nullptr);
  session.IsReadOnly = true;
  session.IsPermanent = true;
  sessionAdd(session);

  // Put session id to user structure to prevent duplicates of session id
  UsersRecord record;
  {
    decltype (UsersCache_)::accessor accessor;
    if (!UsersCache_.find(accessor, tokenInfo.Login)) {
      callback("", "unknown_login");
      return;
    }

    accessor->second.MonitoringSessionId = session.Id.getHexLE();
    record = accessor->second;
  }

  UsersDb_.put(record);
  callback(session.Id.getHexLE(), "ok");
}

void UserManager::updateCredentialsImpl(const std::string &sessionId, const std::string &targetLogin, const Credentials &credentials, Task::DefaultCb callback)
{
  UserWithAccessRights tokenInfo;
  if (!validateSession(sessionId, targetLogin, tokenInfo, true)) {
    callback("unknown_id");
    return;
  }

  UsersRecord record;

  {
    decltype (UsersCache_)::accessor accessor;
    if (!UsersCache_.find(accessor, tokenInfo.Login)) {
      callback("unknown_login");
      return;
    }

    accessor->second.Name = credentials.Name;
    record = accessor->second;
  }

  UsersDb_.put(record);
  callback("ok");
}

void UserManager::updateSettingsImpl(
  const std::string &login,
  const std::string &coin,
  const std::optional<CSettingsPayout> &payout,
  const std::optional<CSettingsMining> &mining,
  const std::optional<CSettingsAutoExchange> &autoExchange,
  const std::string &totp,
  Task::DefaultCb callback)
{
  // Validate coin
  auto backendIt = Backends_.find(coin);
  if (backendIt == Backends_.end()) {
    callback("unknown_coin");
    return;
  }

  // PPS mode requires global PPS to be enabled for this coin
  if (mining.has_value() &&
      mining->MiningMode == EMiningMode::PPS &&
      !backendIt->second->accountingDb()->backendSettings().PPSConfig.Enabled) {
    callback("pps_not_available");
    return;
  }

  // Minimal payout must not be less than the pool's instant minimal payout
  if (payout.has_value() && !payout->MinimalPayout.isZero()) {
    auto payoutConfig = backendIt->second->accountingDb()->backendSettings().PayoutConfig;
    if (payout->MinimalPayout < payoutConfig.InstantMinimalPayout) {
      callback("minimal_payout_too_low");
      return;
    }
  }

  // check 2fa
  {
    decltype (UsersCache_)::accessor accessor;
    if (!UsersCache_.find(accessor, login)) {
      callback("unknown_login");
      return;
    }

    if (!check2fa(accessor->second.TwoFactorAuthData, totp)) {
      callback("2fa_invalid");
      return;
    }
  }

  std::string key = login;
  key.push_back('\0');
  key.append(coin);

  UserSettingsRecord settings;
  {
    decltype (SettingsCache_)::accessor accessor;
    if (SettingsCache_.find(accessor, key)) {
      settings = accessor->second;
    } else {
      settings.Login = login;
      settings.Coin = coin;
    }

    // Merge provided parts
    if (payout.has_value())
      settings.Payout = *payout;
    if (mining.has_value())
      settings.Mining = *mining;
    if (autoExchange.has_value())
      settings.AutoExchange = *autoExchange;

    if (SettingsCache_.find(accessor, key))
      accessor->second = settings;
    else
      SettingsCache_.insert(std::make_pair(key, settings));
  }

  UserSettingsDb_.put(settings);
  LOG_F(INFO,
        "update %s/%s settings: address=%s",
        settings.Login.c_str(),
        settings.Coin.c_str(),
        settings.Payout.Address.c_str());

  // Notify backend about settings update
  backendIt->second->sendUserSettingsUpdate(settings);

  callback("ok");
}

void UserManager::enumerateUsersImpl(const std::string &sessionId, EnumerateUsersTask::Cb callback)
{
  std::vector<Credentials> result;
  UserWithAccessRights tokenInfo;
  if (!validateSession(sessionId, "", tokenInfo, false)) {
    callback("unknown_id", result);
    return;
  }

  if (tokenInfo.Login == "admin" || tokenInfo.Login == "observer") {
    for (const auto &record: UsersCache_) {
      Credentials &credentials = result.emplace_back();
      credentials.Login = record.second.Login;
      credentials.Name = record.second.Name;
      credentials.EMail = record.second.EMail;
      credentials.RegistrationDate = record.second.RegistrationDate;
      credentials.IsActive = record.second.IsActive;
      credentials.IsReadOnly = record.second.IsReadOnly;
      credentials.FeePlan = record.second.FeePlanId;
    }
  } else {
    std::unordered_set<std::string> linkedFeePlans;
    collectLinkedFeePlans(tokenInfo.Login, linkedFeePlans);
    for (const auto &record: UsersCache_) {
      if (!linkedFeePlans.count(record.second.FeePlanId))
        continue;

      Credentials &credentials = result.emplace_back();
      credentials.Login = record.second.Login;
      credentials.Name = record.second.Name;
      credentials.EMail = record.second.EMail;
      credentials.RegistrationDate = record.second.RegistrationDate;
      credentials.IsActive = record.second.IsActive;
      credentials.IsReadOnly = record.second.IsReadOnly;
      credentials.FeePlan = record.second.FeePlanId;
    }
  }

  callback("ok", result);
}

void UserManager::createFeePlanImpl(const std::string &sessionId, const std::string &feePlanId, Task::DefaultCb callback)
{
  UserWithAccessRights tokenInfo;
  if (!validateSession(sessionId, "", tokenInfo, true) || tokenInfo.Login != "admin") {
    callback("unknown_id");
    return;
  }

  if (FeePlanCache_.count(feePlanId)) {
    callback("fee_plan_already_exists");
    return;
  }

  FeePlan plan;
  plan.Modes.resize(static_cast<size_t>(EMiningMode::Count));
  FeePlanCache_.insert(std::make_pair(feePlanId, plan));

  UserFeePlanRecord record;
  record.FeePlanId = feePlanId;
  record.Modes.resize(static_cast<size_t>(EMiningMode::Count));
  UserFeePlanDb_.put(record);

  LOG_F(INFO, "UserManager: created fee plan %s", feePlanId.c_str());
  callback("ok");
}

void UserManager::updateFeePlanImpl(const std::string &sessionId, const std::string &feePlanId, EMiningMode mode, const CModeFeeConfig &config, Task::DefaultCb callback)
{
  UserWithAccessRights tokenInfo;
  if (!validateSession(sessionId, "", tokenInfo, true) || tokenInfo.Login != "admin") {
    callback("unknown_id");
    return;
  }

  // Read current plan from cache
  UserFeePlanRecord record;
  {
    decltype(FeePlanCache_)::const_accessor accessor;
    if (!FeePlanCache_.find(accessor, feePlanId)) {
      callback("unknown_fee_plan");
      return;
    }
    buildFeePlanRecord(accessor->first, accessor->second, record);
  }

  // Replace specified mode
  size_t modeIndex = static_cast<size_t>(mode);
  if (modeIndex >= record.Modes.size())
    record.Modes.resize(modeIndex + 1);
  record.Modes[modeIndex] = config;

  // Validate and update cache
  std::string error = "ok";
  if (acceptFeePlanRecord(record, error)) {
    UserFeePlanDb_.put(record);
    // Notify all backends about fee plan change for the updated mode
    for (const auto &[coinName, backend] : Backends_)
      backend->sendFeePlanUpdate(feePlanId, mode, getFeeRecord(feePlanId, mode, coinName));
  }
  callback(error.c_str());
}

void UserManager::deleteFeePlanImpl(const std::string &sessionId, const std::string &feePlanId, Task::DefaultCb callback)
{
  UserWithAccessRights tokenInfo;
  if (!validateSession(sessionId, "", tokenInfo, true) || tokenInfo.Login != "admin") {
    callback("unknown_id");
    return;
  }

  if (feePlanId == "default") {
    callback("cant_delete_default_plan");
    return;
  }

  if (!FeePlanCache_.count(feePlanId)) {
    callback("unknown_fee_plan");
    return;
  }

  // Collect affected users
  std::vector<std::string> affectedUsers;
  for (const auto &entry: UsersCache_) {
    if (entry.second.FeePlanId == feePlanId)
      affectedUsers.push_back(entry.first);
  }

  // Reset each to "default"
  for (const auto &login: affectedUsers) {
    decltype(UsersCache_)::accessor accessor;
    if (UsersCache_.find(accessor, login)) {
      accessor->second.FeePlanId = "default";
      UsersDb_.put(accessor->second);
    }
  }

  // Remove referral ID from map
  {
    decltype(FeePlanCache_)::const_accessor accessor;
    if (FeePlanCache_.find(accessor, feePlanId) && !accessor->second.ReferralId.isNull())
      ReferralIdMap_.erase(accessor->second.ReferralId);
  }

  // Delete from cache and DB
  FeePlanCache_.erase(feePlanId);
  UserFeePlanRecord record;
  record.FeePlanId = feePlanId;
  UserFeePlanDb_.deleteRow(record);

  // Notify all backends about affected users reset to default, then delete the plan
  for (const auto &[coinName, backend] : Backends_) {
    for (const auto &login : affectedUsers)
      backend->sendUserFeePlanChange(login, "default");
    backend->sendFeePlanDelete(feePlanId);
  }

  LOG_F(INFO, "UserManager: deleted fee plan %s, reset %zu users to default", feePlanId.c_str(), affectedUsers.size());
  callback("ok");
}

void UserManager::changeFeePlanImpl(const std::string &sessionId, const std::string &targetLogin, const std::string &newFeePlan, Task::DefaultCb callback)
{
  UserWithAccessRights tokenInfo;
  if (!validateSession(sessionId, targetLogin, tokenInfo, true)) {
    callback("unknown_id");
    return;
  }

  if (!FeePlanCache_.count(newFeePlan)) {
    callback("fee_plan_not_exists");
    return;
  }

  UsersRecord record;
  {
    decltype (UsersCache_)::accessor accessor;
    if (!UsersCache_.find(accessor, tokenInfo.Login)) {
      callback("unknown_login");
      return;
    }

    accessor->second.FeePlanId = newFeePlan;
    record = accessor->second;
  }

  UsersDb_.put(record);

  // Notify all backends about user's fee plan change
  for (const auto &[coinName, backend] : Backends_)
    backend->sendUserFeePlanChange(tokenInfo.Login, newFeePlan);

  callback("ok");
}

void UserManager::renewFeePlanReferralIdImpl(
  const std::string &sessionId,
  const std::string &feePlanId,
  RenewFeePlanReferralIdTask::Cb callback)
{
  UserWithAccessRights tokenInfo;
  if (!validateSession(sessionId, "", tokenInfo, true) || tokenInfo.Login != "admin") {
    callback("unknown_id", "");
    return;
  }

  decltype(FeePlanCache_)::accessor accessor;
  if (!FeePlanCache_.find(accessor, feePlanId)) {
    callback("unknown_fee_plan", "");
    return;
  }

  // Remove old referral ID from map
  if (!accessor->second.ReferralId.isNull())
    ReferralIdMap_.erase(accessor->second.ReferralId);

  // Generate new referral ID
  BaseBlob<256> newId;
  makeRandom(newId);
  accessor->second.ReferralId = newId;
  ReferralIdMap_[newId] = feePlanId;

  // Persist to DB
  UserFeePlanRecord record;
  buildFeePlanRecord(feePlanId, accessor->second, record);
  UserFeePlanDb_.put(record);

  LOG_F(INFO, "UserManager: renewed referral ID for fee plan %s: %s", feePlanId.c_str(), newId.getHexRaw().c_str());
  callback("ok", newId.getHexRaw());
}

void UserManager::activate2faInitiateImpl(const std::string &sessionId, const std::string &targetLogin, Activate2faInitiateTask::Cb callback)
{
  UserWithAccessRights tokenInfo;
  if (!validateSession(sessionId, targetLogin, tokenInfo, true)) {
    callback("unknown_id", "");
    return;
  }

  // Don't allow special users
  if (tokenInfo.Login == "admin" || tokenInfo.Login == "observer") {
    callback("unknown_login", "");
    return;
  }

  // Check current 2fa status
  std::string emailAddress;
  {
    decltype (UsersCache_)::accessor accessor;
    if (!UsersCache_.find(accessor, tokenInfo.Login)) {
      callback("unknown_login", "");
      return;
    }

    if (!accessor->second.TwoFactorAuthData.empty()) {
      callback("2fa_already_activated", "");
      return;
    }

    emailAddress = accessor->second.EMail;
  }

  // Generate new 2fa key
  uint8_t key[16];
  char keyBase32[128] = {0};
  RAND_bytes(key, sizeof(key));
  base32_encode(key, sizeof(key), reinterpret_cast<uint8_t*>(keyBase32), sizeof(keyBase32));

  // Create action
  UserActionRecord actionRecord;
  makeRandom(actionRecord.Id);
  actionRecord.Login = tokenInfo.Login;
  actionRecord.Type = UserActionRecord::UserTwoFactorActivate;
  actionRecord.TwoFactorKey = keyBase32;
  actionRecord.CreationDate = time(nullptr);

  // Send email
  std::string emailSendError;
  if (SMTP.Enabled && !sendMail(tokenInfo.Login, emailAddress, "Activate two factor authentication at ", BaseCfg.Activate2faLinkPrefix, actionRecord.Id, "For enable two factor authentication", emailSendError)) {
    callback(emailSendError.c_str(), "");
    return;
  }

  actionAdd(actionRecord);
  callback("ok", reinterpret_cast<const char*>(keyBase32));
}

void UserManager::deactivate2faInitiateImpl(const std::string &sessionId, const std::string &targetLogin, Task::DefaultCb callback)
{
  UserWithAccessRights tokenInfo;
  if (!validateSession(sessionId, targetLogin, tokenInfo, true)) {
    callback("unknown_id");
    return;
  }

  // Don't allow special users
  if (tokenInfo.Login == "admin" || tokenInfo.Login == "observer") {
    callback("unknown_login");
    return;
  }

  // Check current 2fa status
  std::string emailAddress;
  {
    decltype (UsersCache_)::accessor accessor;
    if (!UsersCache_.find(accessor, tokenInfo.Login)) {
      callback("unknown_login");
      return;
    }

    if (accessor->second.TwoFactorAuthData.empty()) {
      callback("2fa_not_activated");
      return;
    }

    emailAddress = accessor->second.EMail;
  }

  // Create action
  UserActionRecord actionRecord;
  makeRandom(actionRecord.Id);
  actionRecord.Login = tokenInfo.Login;
  actionRecord.Type = UserActionRecord::UserTwoFactorDeactivate;
  actionRecord.CreationDate = time(nullptr);

  // Send email
  std::string emailSendError;
  if (SMTP.Enabled && !sendMail(tokenInfo.Login, emailAddress, "Deactivate two factor authentication at ", BaseCfg.Deactivate2faLinkPrefix, actionRecord.Id, "For drop two factor authentication", emailSendError)) {
    callback(emailSendError.c_str());
    return;
  }

  actionAdd(actionRecord);
  callback("ok");
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

bool UserManager::validateSession(const std::string &id, const std::string &targetLogin, UserWithAccessRights &result, bool needWriteAccess)
{
  result.IsReadOnly = false;
  time_t currentTime = time(nullptr);
  {
    decltype (SessionsCache_)::accessor accessor;
    if (SessionsCache_.find(accessor, BaseBlob<512>::fromHexLE(id.c_str()))) {
      result.Login = accessor->second.Login;
      result.IsReadOnly = accessor->second.IsReadOnly;
      accessor->second.updateLastAccessTime(currentTime);
    } else {
      return false;
    }
  }

  if (result.IsReadOnly && needWriteAccess)
    return false;

  bool isAdmin = result.Login == "admin";
  bool isObserver = result.Login == "observer" && !needWriteAccess;
  bool isSuperUser = isAdmin || isObserver;
  if (isSuperUser) {
    result.IsReadOnly = isObserver;
    if (!targetLogin.empty()) {
      result.Login = targetLogin;
      return UsersCache_.count(targetLogin);
    } else {
      return true;
    }
  } else if (!targetLogin.empty()) {
    result.IsReadOnly = true;
    std::unordered_set<std::string> linkedFeePlans;
    collectLinkedFeePlans(result.Login, linkedFeePlans);

    decltype (UsersCache_)::const_accessor accessor;
    if (!UsersCache_.find(accessor, targetLogin))
      return false;

    if (linkedFeePlans.count(accessor->second.FeePlanId) && !needWriteAccess) {
      result.Login = targetLogin;
      return true;
    } else {
      return false;
    }
  } else {
    return true;
  }
}

bool UserManager::getUserCredentials(const std::string &login, Credentials &out)
{
  decltype (UsersCache_)::const_accessor accessor;
  if (UsersCache_.find(accessor, login)) {
    out.Name = accessor->second.Name;
    out.EMail = accessor->second.EMail;
    out.RegistrationDate = accessor->second.RegistrationDate;
    out.IsActive = accessor->second.IsActive;
    out.IsReadOnly = accessor->second.IsReadOnly;
    out.HasTwoFactor = !accessor->second.TwoFactorAuthData.empty();
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

std::vector<UserSettingsRecord> UserManager::getAllCoinSettings(const std::string &coin)
{
  std::vector<UserSettingsRecord> result;
  for (auto it = SettingsCache_.begin(); it != SettingsCache_.end(); ++it) {
    if (it->second.Coin == coin)
      result.push_back(it->second);
  }
  return result;
}

void UserManager::fillUserCoinSettings(const std::string &coin, std::unordered_map<std::string, UserSettingsRecord> &out)
{
  for (auto it = SettingsCache_.begin(); it != SettingsCache_.end(); ++it) {
    if (it->second.Coin == coin)
      out.emplace(it->second.Login, it->second);
  }
}

void UserManager::adjustInstantMinimalPayout(const std::string &coin, const UInt<384> &minimalPayout)
{
  for (auto it = SettingsCache_.begin(); it != SettingsCache_.end(); ++it) {
    if (it->second.Coin != coin)
      continue;
    if (it->second.Payout.MinimalPayout.isZero() || it->second.Payout.MinimalPayout >= minimalPayout)
      continue;

    decltype(SettingsCache_)::accessor accessor;
    if (SettingsCache_.find(accessor, it->first) && accessor->second.Payout.MinimalPayout < minimalPayout) {
      LOG_F(INFO,
            "User %s/%s MinimalPayout adjusted to pool minimum",
            accessor->second.Login.c_str(),
            coin.c_str());
      accessor->second.Payout.MinimalPayout = minimalPayout;
      UserSettingsDb_.put(accessor->second);
    }
  }
}

std::string UserManager::getFeePlanId(const std::string &login)
{
  static const std::string defaultPlan = "default";
  decltype (UsersCache_)::const_accessor accessor;
  if (UsersCache_.find(accessor, login))
    return !accessor->second.FeePlanId.empty() ? accessor->second.FeePlanId : defaultPlan;
  else
    return "default";
}

bool UserManager::queryFeePlan(
  const std::string &sessionId,
  const std::string &feePlanId,
  EMiningMode mode,
  std::string &status,
  BaseBlob<256> &referralIdOut,
  CModeFeeConfig &result)
{
  UserWithAccessRights tokenInfo;
  if (!validateSession(sessionId, "", tokenInfo, false) || (tokenInfo.Login != "admin" && tokenInfo.Login != "observer")) {
    status = "unknown_id";
    return false;
  }

  decltype(FeePlanCache_)::const_accessor accessor;
  if (!FeePlanCache_.find(accessor, feePlanId)) {
    status = "unknown_fee_plan";
    return false;
  }

  size_t modeIndex = static_cast<size_t>(mode);
  if (modeIndex < accessor->second.Modes.size())
    buildModeFeeConfig(accessor->second.Modes[modeIndex], result);

  referralIdOut = accessor->second.ReferralId;

  status = "ok";
  return true;
}

bool UserManager::enumerateFeePlan(const std::string &sessionId, std::string &status, std::vector<std::string> &result)
{
  UserWithAccessRights tokenInfo;
  if (!validateSession(sessionId, "", tokenInfo, false) || (tokenInfo.Login != "admin" && tokenInfo.Login != "observer")) {
    status = "unknown_id";
    return false;
  }

  for (const auto &entry: FeePlanCache_)
    result.push_back(entry.first);
  std::sort(result.begin(), result.end());

  status = "ok";
  return true;
}

// Returns flat fee list for a given plan/mode/coin (no fee chain resolution)
std::vector<UserFeePair> UserManager::getFeeRecord(const std::string &feePlanId, EMiningMode mode, const std::string &coin)
{
  decltype (FeePlanCache_)::const_accessor accessor;
  if (FeePlanCache_.find(accessor, feePlanId)) {
    const auto &plan = accessor->second;
    if (static_cast<size_t>(mode) < plan.Modes.size()) {
      const auto &modeCfg = plan.Modes[static_cast<size_t>(mode)];
      auto It = modeCfg.CoinSpecific.find(coin);
      return It != modeCfg.CoinSpecific.end() ? It->second : modeCfg.Default;
    }
  }
  return {};
}

std::vector<std::pair<std::string, std::vector<UserFeePair>>> UserManager::getAllFeeRecords(EMiningMode mode, const std::string &coin)
{
  std::vector<std::pair<std::string, std::vector<UserFeePair>>> result;
  for (const auto &entry : FeePlanCache_)
    result.emplace_back(entry.first, getFeeRecord(entry.first, mode, coin));
  return result;
}

void UserManager::fillUserFeePlanIds(std::unordered_map<std::string, std::string> &out)
{
  for (const auto &entry : UsersCache_) {
    const std::string &feePlanId = !entry.second.FeePlanId.empty() ? entry.second.FeePlanId : "default";
    out.emplace(entry.first, feePlanId);
  }
}
