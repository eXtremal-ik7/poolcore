#pragma once

#include "common.h"
#include "stratumMsg.h"
#include "poolcommon/arith_uint256.h"
#include "poolcommon/debug.h"
#include "poolcommon/jsonSerializer.h"
#include "poolcore/backend.h"
#include "poolcore/poolCore.h"
#include "poolcore/poolInstance.h"
#include <openssl/rand.h>
#include <rapidjson/writer.h>
#include <unordered_map>

enum StratumErrorTy {
  StratumErrorInvalidShare = 20,
  StratumErrorJobNotFound = 21,
  StratumErrorDuplicateShare = 22,
  StratumErrorUnauthorizedWorker = 24
};

static inline const char *stratumErrorText(StratumErrorTy error)
{
  switch (error) {
    case StratumErrorInvalidShare : return "invalid_share";
    case StratumErrorJobNotFound : return "stale_share";
    case StratumErrorDuplicateShare : return "duplicate_share";
    case StratumErrorUnauthorizedWorker : return "unauthorized_worker";
    default : return "error";
  }
}

template<typename T>
static inline unsigned popcount(T number)
{
  unsigned count = 0;
  for(; number; count++)
    number &= number-1;
  return count;
}

static inline void addId(JSON::Object &object, StratumMessage &msg) {
  if (!msg.stringId.empty())
    object.addString("id", msg.stringId);
  else
    object.addInt("id", msg.integerId);
}

template<typename X>
class StratumInstance : public CPoolInstance {
public:
  StratumInstance(asyncBase *monitorBase, UserManager &userMgr, CThreadPool &threadPool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) : CPoolInstance(monitorBase, userMgr, threadPool), CurrentThreadId_(0) {
    Name_ = (std::string)X::Proto::TickerName + ".stratum";
    Data_.reset(new ThreadData[threadPool.threadsNum()]);

    unsigned totalInstancesNum = instancesNum * threadPool.threadsNum();
    for (unsigned i = 0; i < threadPool.threadsNum(); i++) {
      // initialInstanceId used for fixed extra nonce part calculation
      unsigned initialInstanceId = instanceId*threadPool.threadsNum() + i;
      Data_[i].ThreadCfg.initialize(initialInstanceId, totalInstancesNum);
      X::Proto::checkConsensusInitialize(Data_[i].CheckConsensusCtx);
      Data_[i].WorkerBase = threadPool.getBase(i);
    }

    if (!config.HasMember("port") || !config["port"].IsUint()) {
      LOG_F(ERROR, "instance %s: can't read 'port' valuee from config", Name_.c_str());
      exit(1);
    }

    uint16_t port = config["port"].GetInt();

    // Share diff
    if (config.HasMember("shareDiff")) {
      if (config["shareDiff"].IsUint64()) {
        ConstantShareDiff_ = config["shareDiff"].GetUint64();
      } else if (config["shareDiff"].IsFloat()) {
        ConstantShareDiff_ = config["shareDiff"].GetFloat();
      } else {
        LOG_F(ERROR, "%s: 'shareDiff' must be an integer", Name_.c_str());
        exit(1);
      }
    } else if (config.HasMember("varDiffTarget") && config.HasMember("minShareDiff")) {
      LOG_F(ERROR, "%s: Vardiff not supported now", Name_.c_str());
      exit(1);
    } else {
      LOG_F(ERROR, "instance %s: no share difficulty config (expected 'shareDiff' for constant difficulty or 'varDiffTarget' and 'minShareDiff' for variable diff", Name_.c_str());
      exit(1);
    }

    // BTC ASIC boost
    if (config.HasMember("versionMask") && config["versionMask"].IsString())
      VersionMask_ = readHexBE<uint32_t>(config["versionMask"].GetString(), 4);

    MiningCfg_.initialize(config);

    // Main listener
    createListener(monitorBase, port, [](socketTy socket, HostAddress address, void *arg) { static_cast<StratumInstance*>(arg)->newFrontendConnection(socket, address); }, this);
  }

  virtual void checkNewBlockTemplate(CBlockTemplate *blockTemplate, PoolBackend *backend) override {
    intrusive_ptr<CBlockTemplate> holder(blockTemplate);
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, blockTemplate, backend));
  }

  virtual void stopWork() override {
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, nullptr, nullptr));
  }

  void acceptWork(unsigned, CBlockTemplate *blockTemplate, PoolBackend *backend) {
    auto beginPt = std::chrono::steady_clock::now();
    unsigned counter = 0;
    if (!blockTemplate) {
      return;
    }

    ThreadData &data = Data_[GetLocalThreadId()];

    auto &backendConfig = backend->getConfig();
    auto &coinInfo = backend->getCoinInfo();
    bool isNewBlock = false;
    if (!data.WorkSet.empty()) {
      if (data.WorkSet.back()->isNewBlock(coinInfo.Name, blockTemplate->UniqueWorkId)) {
        // Incoming block template contains new work
        // Cleanup work set and push last known work (for reuse allocated memory or keep non changed work part)
        isNewBlock = true;
        typename X::Stratum::Work *lastWork = data.WorkSet.back().release();
        data.WorkSet.clear();
        data.WorkSet.emplace_back(lastWork);
        data.MinorLastWorkId_ = 0;
      } else {
        // Deep copy last work (required for merged mining)
        typename X::Stratum::Work *clone = new typename X::Stratum::Work(*data.WorkSet.back());
        data.WorkSet.emplace_back(clone);
        data.MinorLastWorkId_++;
      }
    } else {
      isNewBlock = true;
      data.WorkSet.emplace_back(new typename X::Stratum::Work);
      data.MinorLastWorkId_ = 0;
    }

    // Get mining address and coinbase message
    typename X::Stratum::Work &work = *data.WorkSet.back();
    typename X::Proto::AddressTy miningAddress;
    const std::string &addr = backendConfig.MiningAddresses.get();
    if (!decodeHumanReadableAddress(addr, coinInfo.PubkeyAddressPrefix, miningAddress)) {
      LOG_F(WARNING, "%s: mining address %s is invalid", coinInfo.Name.c_str(), addr.c_str());
      return;
    }

    std::string error;
    if (!work.loadFromTemplate(blockTemplate->Document, blockTemplate->UniqueWorkId, MiningCfg_, backend, coinInfo.Name, miningAddress, backendConfig.CoinBaseMsg.data(), backendConfig.CoinBaseMsg.size(), error)) {
      LOG_F(ERROR, "%s: can't process block template; error: %s", Name_.c_str(), error.c_str());
      return;
    }

    if (isNewBlock) {
      // Update major job id
      uint64_t newJobId = time(nullptr);
      data.MajorWorkId_ = (data.MajorWorkId_ == newJobId) ? newJobId+1 : newJobId;
    }

    if (work.initialized()) {
      work.buildNotifyMessage(MiningCfg_, data.MajorWorkId_, data.MinorLastWorkId_, isNewBlock);
      int64_t currentTime = time(nullptr);
      for (auto &connection: data.Connections_) {
        stratumSendWork(connection, currentTime);
        counter++;
      }
    }

    while (data.WorkSet.size() > WorksetSizeLimit)
      data.WorkSet.pop_front();

    auto endPt = std::chrono::steady_clock::now();
    auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(endPt - beginPt).count();
    LOG_F(INFO, "%s: Accepting work %" PRIu64 "#%zu (reset=%s) & send to %u clients in %.3lf seconds", Name_.c_str(), data.MajorWorkId_, data.MinorLastWorkId_, isNewBlock ? "yes" : "no", counter, static_cast<double>(timeDiff)/1000.0);
  }

private:
  class AcceptWork : public CThreadPool::Task {
  public:
    AcceptWork(StratumInstance &instance, CBlockTemplate *blockTemplate, PoolBackend *backend) : Instance_(instance), BlockTemplate_(blockTemplate), Backend_(backend) {}
    void run(unsigned workerId) final { Instance_.acceptWork(workerId, BlockTemplate_.get(), Backend_); }
  private:
    StratumInstance &Instance_;
    intrusive_ptr<CBlockTemplate> BlockTemplate_;
    PoolBackend *Backend_;
  };

  struct Worker {
    std::string User;
    std::string WorkerName;
  };

  struct Connection {
    Connection(StratumInstance *instance, aioObject *socket, unsigned workerId, HostAddress address) : Instance(instance), Socket(socket), WorkerId(workerId), Address(address) {
      struct in_addr addr;
      addr.s_addr = Address.ipv4;
      AddressHr = inet_ntoa(addr);
      AddressHr.push_back(':');
      AddressHr.append(std::to_string(htons(address.port)));
    }

    ~Connection() {
      if (isDebugInstanceStratumConnections())
        LOG_F(1, "%s: disconnected from %s", Instance->Name_.c_str(), AddressHr.c_str());
      Instance->Data_[WorkerId].Connections_.erase(this);
      deleteAioObject(Socket);
    }

    bool Initialized = false;
    StratumInstance *Instance;
    // Network
    aioObject *Socket;
    unsigned WorkerId;
    HostAddress Address;
    std::string AddressHr;
    bool Active = true;
    bool IsCgMiner = false;
    int64_t LastUpdateTime = std::numeric_limits<int64_t>::max();
    // Stratum protocol decoding
    char Buffer[40960];
    unsigned MsgTailSize = 0;
    // Mining info
    typename X::Stratum::WorkerConfig WorkerConfig;
    // Current share difficulty (one for all workers on connection)
    double ShareDifficulty;
    // Workers
    std::unordered_map<std::string, Worker> Workers;
  };

  struct ThreadData {
    asyncBase *WorkerBase;
    typename X::Proto::CheckConsensusCtx CheckConsensusCtx;
    typename X::Stratum::ThreadConfig ThreadCfg;
    std::deque<std::unique_ptr<typename X::Stratum::Work>> WorkSet;
    std::set<Connection*> Connections_;
    uint64_t MajorWorkId_ = 0;
    uint64_t MinorLastWorkId_ = 0;
  };

private:
  void send(Connection *connection, const xmstream &stream) {
    if (isDebugInstanceStratumMessages()) {
      std::string msg(stream.data<char>(), stream.sizeOf());
      LOG_F(1, "%s(%s): outgoing message %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), msg.c_str());
    }
    aioWrite(connection->Socket, stream.data(), stream.sizeOf(), afWaitAll, 0, nullptr, nullptr);
  }

  void onStratumSubscribe(Connection *connection, StratumMessage &msg) {
    if (msg.subscribe.minerUserAgent.find("cgminer") != std::string::npos)
      connection->IsCgMiner = true;

    char buffer[4096];
    xmstream stream(buffer, sizeof(buffer));
    stream.reset();
    connection->WorkerConfig.onSubscribe(MiningCfg_, msg, stream);
    send(connection, stream);
  }

  bool onStratumAuthorize(Connection *connection, StratumMessage &msg) {
    bool authSuccess = false;
    std::string error;
    size_t dotPos = msg.authorize.login.find('.');
    if (dotPos != msg.authorize.login.npos) {
      Worker worker;
      worker.User.assign(msg.authorize.login.begin(), msg.authorize.login.begin() + dotPos);
      worker.WorkerName.assign(msg.authorize.login.begin() + dotPos + 1, msg.authorize.login.end());
      connection->Workers.insert(std::make_pair(msg.authorize.login, worker));
      authSuccess = UserMgr_.checkUser(worker.User);
    } else {
      error = "Invalid user name format (username.workername required)";
    }

    char buffer[4096];
    xmstream stream(buffer, sizeof(buffer));
    stream.reset();
    {
      JSON::Object object(stream);
      addId(object, msg);
      object.addBoolean("result", authSuccess);
      if (authSuccess)
        object.addNull("error");
      else
        object.addString("error", error);
    }
    stream.write('\n');
    send(connection, stream);
    return authSuccess;
  }

  bool onStratumMiningConfigure(Connection *connection, StratumMessage &msg) {
    xmstream stream;
    std::string error;
    {
      JSON::Object object(stream);
      addId(object, msg);
      object.addField("result");
      {
        JSON::Object result(stream);
        if (msg.miningConfigure.ExtensionsField & StratumMiningConfigure::EVersionRolling) {
          bool versionRolling = false;
          if (msg.miningConfigure.VersionRollingMask.has_value() && msg.miningConfigure.VersionRollingMask == 0xFFFFFFFF)
            msg.miningConfigure.VersionRollingMinBitCount = 2;
          if (msg.miningConfigure.VersionRollingMask.has_value() && msg.miningConfigure.VersionRollingMinBitCount.has_value()) {
            // Calculate target bit mask
            uint32_t targetBitMask = msg.miningConfigure.VersionRollingMask.value() & VersionMask_;
            // Get target bit count
            // TODO: use std::popcount since C++20
            unsigned count = popcount(targetBitMask);
            if (count >= msg.miningConfigure.VersionRollingMinBitCount.value()) {
              result.addString("version-rolling.mask", writeHexBE(targetBitMask, 4));
              connection->WorkerConfig.setupVersionRolling(targetBitMask);
              versionRolling = true;
            }
          }

          if (!versionRolling) {
            struct in_addr addr;
            addr.s_addr = connection->Address.ipv4;
            // TODO: change to DEBUG
            LOG_F(WARNING,
                  "%s: can't setup version rolling for %s (client mask: %X; minimal bit count: %X, server mask: %X)",
                  connection->Instance->Name_.c_str(),
                  inet_ntoa(addr),
                  msg.miningConfigure.VersionRollingMask.has_value() ? msg.miningConfigure.VersionRollingMask.value() : 0,
                  msg.miningConfigure.VersionRollingMinBitCount.has_value() ? msg.miningConfigure.VersionRollingMinBitCount.value() : 0,
                  VersionMask_);
          }
          result.addBoolean("version-rolling", versionRolling);
        }

        if (msg.miningConfigure.ExtensionsField & StratumMiningConfigure::EMinimumDifficulty) {
          bool minimumDifficulty = false;
          result.addBoolean("minimum-difficulty", minimumDifficulty);
        }

        if (msg.miningConfigure.ExtensionsField & StratumMiningConfigure::ESubscribeExtraNonce) {
          bool subscribeExtraNonce = false;
          result.addBoolean("subscribe-extranonce", subscribeExtraNonce);
        }
      }

      object.addNull("error");
    }

    stream.write('\n');
    send(connection, stream);

    ThreadData &data = Data_[GetLocalThreadId()];
    if (connection->IsCgMiner && !data.WorkSet.empty())
      stratumSendWork(connection, time(nullptr));
    return true;
  }

  void onStratumMiningSuggestDifficulty(Connection *connection, StratumMessage &message) {
    // Nothing to do
  }

  void onStratumExtraNonceSubscribe(Connection *connection, StratumMessage &message) {
    xmstream stream;
    {
      JSON::Object object(stream);
      addId(object, message);
      object.addBoolean("result", true);
      object.addNull("error");
    }

    stream.write('\n');
    send(connection, stream);
  }

  bool shareCheck(Connection *connection, StratumMessage &msg, StratumErrorTy &errorCode) {
    ThreadData &data = Data_[GetLocalThreadId()];

    // Check worker name
    auto It = connection->Workers.find(msg.submit.WorkerName);
    if (It == connection->Workers.end()) {
      if (isDebugInstanceStratumRejects())
        LOG_F(1, "%s(%s) reject: unknown worker name: %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), msg.submit.WorkerName.c_str());
      errorCode = StratumErrorUnauthorizedWorker;
      return false;
    }
    Worker &worker = It->second;

    // Check job id
    size_t localJobIndex;
    {
      size_t sharpPos = msg.submit.JobId.find('#');
      if (sharpPos == msg.submit.JobId.npos) {
        if (isDebugInstanceStratumRejects())
          LOG_F(1, "%s(%s) %s/%s reject: invalid job id format: %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), worker.User.c_str(), worker.WorkerName.c_str(), msg.submit.JobId.c_str());
        errorCode = StratumErrorJobNotFound;
        return false;
      }

      msg.submit.JobId[sharpPos] = 0;
      uint64_t majorJobId = xatoi<uint64_t>(msg.submit.JobId.c_str());
      size_t jobIndex = xatoi<size_t>(msg.submit.JobId.c_str() + sharpPos + 1);
      size_t jobIdOffset = data.MinorLastWorkId_ - data.WorkSet.size() + 1;
      localJobIndex = jobIndex - jobIdOffset;
      bool hasJob = jobIndex >= jobIdOffset && localJobIndex < data.WorkSet.size();
      if (majorJobId != data.MajorWorkId_ || !hasJob) {
        if (isDebugInstanceStratumRejects())
          LOG_F(1, "%s(%s) %s/%s reject: unknown job id: %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), worker.User.c_str(), worker.WorkerName.c_str(), msg.submit.JobId.c_str());
        errorCode = StratumErrorJobNotFound;
        return false;
      }
    }

    // Build header and check proof of work
    std::string user = worker.User;
    typename X::Stratum::Work &work = *data.WorkSet[localJobIndex];
    if (!work.prepareForSubmit(connection->WorkerConfig, MiningCfg_, msg)) {
      if (isDebugInstanceStratumRejects())
        LOG_F(1, "%s(%s) %s/%s reject: invalid share format", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), worker.User.c_str(), worker.WorkerName.c_str());
      errorCode = StratumErrorInvalidShare;
      return false;
    }

    if (!work.checkForDuplicate(worker.User + "." + worker.WorkerName, connection->WorkerConfig)) {
      if (isDebugInstanceStratumRejects())
        LOG_F(1, "%s(%s) %s/%s reject: duplicate share", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), worker.User.c_str(), worker.WorkerName.c_str());
      errorCode = StratumErrorDuplicateShare;
      return false;
    }

    // Loop over all affected backends (usually 1 or 2 with enabled merged mining)
    bool shareAccepted = false;
    for (size_t i = 0, ie = work.backendsNum(); i != ie; ++i) {
      PoolBackend *backend = work.backend(i);
      if (!backend) {
        if (isDebugInstanceStratumRejects())
          LOG_F(1, "%s(%s) %s/%s sub-reject: backend %zu not initialized", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), worker.User.c_str(), worker.WorkerName.c_str(), i);
        continue;
      }

      uint64_t height = work.height(i);
      double shareDiff = 0.0;
      bool isBlock = work.checkConsensus(i, &shareDiff);
      if (shareDiff < connection->ShareDifficulty) {
        if (isDebugInstanceStratumRejects())
          LOG_F(1,
                "%s(%s) %s/%s sub-reject(%s): invalid share difficulty %lg (%lg required)",
                connection->Instance->Name_.c_str(),
                connection->AddressHr.c_str(),
                worker.User.c_str(),
                worker.WorkerName.c_str(),
                backend->getCoinInfo().Name.c_str(),
                shareDiff,
                connection->ShareDifficulty);
        continue;
      }

      shareAccepted = true;
      if (isBlock) {
        std::string blockHash = work.hash(i);
        LOG_F(INFO, "%s: new proof of work for %s found; hash: %s; transactions: %zu", Name_.c_str(), backend->getCoinInfo().Name.c_str(), blockHash.c_str(), work.txNum(i));

        // Serialize block
        int64_t generatedCoins = work.blockReward(i);
        double shareDifficulty = connection->ShareDifficulty;
        CNetworkClientDispatcher &dispatcher = backend->getClientDispatcher();
        dispatcher.aioSubmitBlock(data.WorkerBase, work.blockHexData(i).data(), work.blockHexData(i).sizeOf(), [height, blockHash, generatedCoins, backend, shareDifficulty, worker](uint32_t successNum, const std::string &hostName, const std::string &error) {
          if (successNum) {
            LOG_F(INFO, "* block %s (%" PRIu64 ") accepted by %s", blockHash.c_str(), height, hostName.c_str());
            if (successNum == 1) {
              // Send share with block to backend
              CShare *backendShare = new CShare;
              backendShare->Time = time(nullptr);
              backendShare->userId = worker.User;
              backendShare->workerId = worker.WorkerName;
              backendShare->height = height;
              // TODO: calculate this value
              backendShare->value = 1;
              backendShare->WorkValue = shareDifficulty;
              backendShare->isBlock = true;
              backendShare->hash = blockHash;
              backendShare->generatedCoins = generatedCoins;
              backend->sendShare(backendShare);
            }
          } else {
            LOG_F(ERROR, "* block %s (%" PRIu64 ") rejected by %s error: %s", blockHash.c_str(), height, hostName.c_str(), error.c_str());
          }
        });
      } else {
        CShare *backendShare = new CShare;
        backendShare->Time = time(nullptr);
        backendShare->userId = worker.User;
        backendShare->workerId = worker.WorkerName;
        backendShare->height = height;
        // TODO: calculate this value
        backendShare->value = 1;
        backendShare->WorkValue = connection->ShareDifficulty;
        backendShare->isBlock = false;
        backend->sendShare(backendShare);
      }
    }

    if (!shareAccepted)
      errorCode = StratumErrorInvalidShare;
    return shareAccepted;
  }

  void onStratumSubmit(Connection *connection, StratumMessage &msg) {
    StratumErrorTy errorCode;
    bool result = shareCheck(connection, msg, errorCode);
    xmstream stream;
    {
      // Response
      JSON::Object object(stream);
      addId(object, msg);
      if (result) {
        object.addBoolean("result", true);
        object.addNull("error");
      } else {
        object.addNull("result");
        object.addField("error");
        {
          JSON::Array error(stream);
          error.addInt(static_cast<int>(errorCode));
          error.addString(stratumErrorText(errorCode));
          error.addNull();
        }
      }
    }

    stream.write('\n');
    send(connection, stream);

    if (!result) {
      if (errorCode == StratumErrorJobNotFound) {
        int64_t timeDiff = time(nullptr) - connection->LastUpdateTime;
        if (timeDiff >= 4) {
          if (isDebugInstanceStratumConnections())
            LOG_F(1, "%s: connection %s too late stale share, disconnecting", Name_.c_str(), connection->AddressHr.c_str());
          connection->Active = false;
        }
      } else {
        if (isDebugInstanceStratumConnections())
          LOG_F(1, "%s: connection %s fatal error, disconnecting", Name_.c_str(), connection->AddressHr.c_str());
        connection->Active = false;
      }
    }
  }

  void onStratumMultiVersion(Connection *connection, StratumMessage &message) {
    xmstream stream;
    {
      JSON::Object object(stream);
      addId(object, message);
      object.addBoolean("result", true);
      object.addNull("error");
    }

    stream.write('\n');
    send(connection, stream);
  }

  void stratumSendTarget(Connection *connection) {
    ThreadData &data = Data_[GetLocalThreadId()];
    xmstream stream;
    {
      JSON::Object object(stream);
      object.addString("method", "mining.set_difficulty");
      object.addField("params");
      {
        JSON::Array params(stream);
        params.addDouble(connection->ShareDifficulty * X::Stratum::DifficultyFactor);
      }
    }

    stream.write('\n');
    send(connection, stream);
  }

  void stratumSendWork(Connection *connection, int64_t currentTime) {
    ThreadData &data = Data_[GetLocalThreadId()];
    typename X::Stratum::Work &work = *data.WorkSet.back();
    connection->LastUpdateTime = currentTime;
    send(connection, work.notifyMessage());
  }

  void newFrontendConnection(socketTy fd, HostAddress address) {
    ThreadData &data = Data_[CurrentThreadId_];
    Connection *connection = new Connection(this, newSocketIo(data.WorkerBase, fd), CurrentThreadId_, address);
    connection->WorkerConfig.initialize(data.ThreadCfg);
    if (isDebugInstanceStratumConnections())
      LOG_F(1, "%s: new connection from %s", Name_.c_str(), connection->AddressHr.c_str());

    // Initialize share difficulty
    connection->ShareDifficulty = ConstantShareDiff_;

    aioRead(connection->Socket, connection->Buffer, sizeof(connection->Buffer), afNone, 3000000, reinterpret_cast<aioCb*>(readCb), connection);
    CurrentThreadId_ = (CurrentThreadId_ + 1) % ThreadPool_.threadsNum();
  }

  static void readCb(AsyncOpStatus status, aioObject*, size_t size, Connection *connection) {
    if (status != aosSuccess) {
      delete connection;
      return;
    }

    ThreadData &data = connection->Instance->Data_[connection->WorkerId];
    if (!connection->Initialized) {
      data.Connections_.insert(connection);
      connection->Initialized = true;
    }

    const char *nextMsgPos;
    const char *p = connection->Buffer;
    const char *e = connection->Buffer + connection->MsgTailSize + size;
    while (p != e && (nextMsgPos = static_cast<const char*>(memchr(p, '\n', e - p)))) {
      // parse stratum message
      bool result = true;
      StratumMessage msg;
      size_t stratumMsgSize = nextMsgPos - p;
      if (isDebugInstanceStratumMessages()) {
        std::string msg(p, stratumMsgSize);
        LOG_F(1, "%s(%s): incoming message %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), msg.c_str());
      }

      switch (decodeStratumMessage(p, stratumMsgSize, &msg)) {
        case StratumDecodeStatusTy::Ok :
          // Process stratum messages here
          switch (msg.method) {
            case StratumMethodTy::Subscribe :
              connection->Instance->onStratumSubscribe(connection, msg);
              break;
            case StratumMethodTy::Authorize : {
              result = connection->Instance->onStratumAuthorize(connection, msg);
              if (result && !data.WorkSet.empty()) {
                connection->Instance->stratumSendTarget(connection);
                connection->Instance->stratumSendWork(connection, time(nullptr));
              }
              break;
            }
            case StratumMethodTy::MiningConfigure :
              connection->Instance->onStratumMiningConfigure(connection, msg);
              break;
            case StratumMethodTy::MiningSuggestDifficulty :
              connection->Instance->onStratumMiningSuggestDifficulty(connection, msg);
              break;
            case StratumMethodTy::ExtraNonceSubscribe :
              connection->Instance->onStratumExtraNonceSubscribe(connection, msg);
              break;
            case StratumMethodTy::Submit :
              connection->Instance->onStratumSubmit(connection, msg);
              break;
            case StratumMethodTy::MultiVersion :
              connection->Instance->onStratumMultiVersion(connection, msg);
              break;
            default : {
              // unknown method
              struct in_addr addr;
              addr.s_addr = connection->Address.ipv4;
              std::string msg(p, stratumMsgSize);
              LOG_F(ERROR, "%s: unknown stratum method received from %s" , connection->Instance->Name_.c_str(), inet_ntoa(addr));
              LOG_F(ERROR, " * message text: %s", msg.c_str());
              break;
            }
          }

          break;
        case StratumDecodeStatusTy::JsonError : {
          std::string msg(p, stratumMsgSize);
          LOG_F(ERROR, "%s(%s): JsonError %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), msg.c_str());
          result = false;
          break;
        }
        case StratumDecodeStatusTy::FormatError : {
          std::string msg(p, stratumMsgSize);
          LOG_F(ERROR, "%s(%s): FormatError %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), msg.c_str());
          result = false;
          break;
        }
        default :
          break;
      }

      if (!result) {
        delete connection;
        return;
      }

      p = nextMsgPos + 1;
    }

    // move tail to begin of buffer
    if (p != e) {
      connection->MsgTailSize = e-p;
      if (connection->MsgTailSize >= sizeof(connection->Buffer)) {
        struct in_addr addr;
        addr.s_addr = connection->Address.ipv4;
        LOG_F(ERROR, "%s: too long stratum message from %s", connection->Instance->Name_.c_str(), inet_ntoa(addr));
        delete connection;
        return;
      }

      memmove(connection->Buffer, p, e-p);
    } else {
      connection->MsgTailSize = 0;
    }

    if (connection->Active)
      aioRead(connection->Socket, connection->Buffer + connection->MsgTailSize, sizeof(connection->Buffer) - connection->MsgTailSize, afNone, 0, reinterpret_cast<aioCb*>(readCb), connection);
    else
      delete connection;
  }

private:
  static constexpr size_t WorksetSizeLimit = 5;
  std::unique_ptr<ThreadData[]> Data_;
  std::string Name_;
  unsigned CurrentThreadId_;
  typename X::Stratum::MiningConfig MiningCfg_;
  double ConstantShareDiff_;

  // ASIC boost 'overt' data
  uint32_t VersionMask_ = 0x1FFFE000;
};
