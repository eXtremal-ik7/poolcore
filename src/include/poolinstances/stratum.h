#pragma once

#include "common.h"
#include "stratumMsg.h"
#include "stratumWorkStorage.h"
#include "poolcommon/debug.h"
namespace jp {
#include "poolcommon/jsonConfigParser.h"
}
#include "poolcommon/jsonSerializer.h"
#include "poolcore/backend.h"
#include "poolcore/blockTemplate.h"
#include "poolcore/poolCore.h"
#include "poolcore/poolInstance.h"
#include "poolcore/shareAccumulator.h"
#include <openssl/rand.h>
#include <rapidjson/writer.h>
#include <unordered_map>

static constexpr unsigned UncheckedSendCount = 32;
static constexpr uint64_t SendTimeout = 4000000;

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

template<typename X>
class StratumInstance : public CPoolInstance {
public:
  using CWork = StratumWork;
  using CSingleWork = StratumSingleWork;
  using CMergedWork = StratumMergedWork;

public:
  StratumInstance(asyncBase *monitorBase,
                  UserManager &userMgr,
                  const std::vector<PoolBackend*> &linkedBackends,
                  CThreadPool &threadPool,
                  StatisticServer *algoMetaStatistic,
                  ComplexMiningStats *miningStats,
                  unsigned instanceId,
                  unsigned instancesNum,
                  rapidjson::Value &config,
                  CPriceFetcher *priceFetcher)
      : CPoolInstance(monitorBase, userMgr, linkedBackends, threadPool, algoMetaStatistic, miningStats),
        CurrentThreadId_(0),
        PriceFetcher_(priceFetcher)
  {
    // Parse config
    jp::EErrorType acc = jp::EOk;
    std::string cfgError;
    unsigned port;
    std::optional<std::string> versionMask;
    std::optional<std::vector<std::string>> workResetBackendNames;
    std::optional<std::vector<std::string>> primaryBackendNames;
    std::optional<std::vector<std::string>> secondaryBackendNames;

    jp::jsonParseUnsignedInt(config, "port", &port, &acc, "", cfgError);
    jp::jsonParseDouble(config, "shareDiff", &ConstantShareDiff_, &acc, "", cfgError);
    jp::jsonParseStringOptional(config, "versionMask", versionMask, &acc, "", cfgError);
    jp::jsonParseBoolean(config, "profitSwitcherEnabled", &ProfitSwitcherEnabled_, std::make_pair(true, false), &acc, "", cfgError);
    jp::jsonParseStringArrayOptional(config, "resetWorkOnBlockChange", workResetBackendNames, &acc, "", cfgError);
    jp::jsonParseStringArrayOptional(config, "primaryBackends", primaryBackendNames, &acc, "", cfgError);
    jp::jsonParseStringArrayOptional(config, "secondaryBackends", secondaryBackendNames, &acc, "", cfgError);
    if (acc != jp::EOk) {
      LOG_F(ERROR, "%s: can't load stratum config", Name_.c_str());
      LOG_F(ERROR, "%s", cfgError.c_str());
      exit(1);
    }

    Name_ += ".";
    Name_ += std::to_string(port);
    if (versionMask.has_value()) {
      // BTC ASIC boost
      if (versionMask.value().size() != 8) {
        LOG_F(ERROR, "versionMask must be 8-byte hex value");
        exit(1);
      }

      VersionMask_ = readHexBE<uint32_t>(versionMask.value().c_str(), 4);
    }

    bool hasRtt = false;
    std::unordered_map<std::string, size_t> backendsMap;
    for (size_t i = 0; i < linkedBackends.size(); i++) {
      backendsMap[linkedBackends[i]->getCoinInfo().Name] = i;
      hasRtt |= linkedBackends[i]->getCoinInfo().HasRtt;
    }

    std::vector<size_t> workResetBackends;
    std::vector<size_t> primaryBackends;
    std::vector<size_t> secondaryBackends;

    buildBackendList(linkedBackends, workResetBackendNames, backendsMap, false, [](const PoolBackend *backend) -> bool {
      return backend->getCoinInfo().ResetWorkOnBlockChange;
    }, workResetBackends, "resetWorkOnBlockChange");
    if (X::Stratum::MergedMiningSupport) {
      buildBackendList(linkedBackends, primaryBackendNames, backendsMap, false, [](const PoolBackend *backend) -> bool {
        return backend->getCoinInfo().CanBePrimaryCoin;
      }, primaryBackends, "primaryBackends");
      buildBackendList(linkedBackends, secondaryBackendNames, backendsMap, false, [](const PoolBackend *backend) -> bool {
        return backend->getCoinInfo().CanBeSecondaryCoin;
      }, secondaryBackends, "secondaryBackends");
    } else {
      for (size_t i = 0; i < linkedBackends.size(); i++)
        primaryBackends.push_back(i);
    }

    if (primaryBackends.empty()) {
      LOG_F(ERROR, "%s: empty primary backends list", Name_.c_str());
      exit(1);
    } else if (primaryBackends.size() > 1 && !ProfitSwitcherEnabled_) {
      LOG_F(ERROR, "%s: too much primary backends specified without profit switcher", Name_.c_str());
      exit(1);
    }

    MainWorkSource_ = primaryBackends[0];

    Data_.reset(new ThreadData[threadPool.threadsNum()]);

    unsigned totalInstancesNum = instancesNum * threadPool.threadsNum();
    for (unsigned i = 0; i < threadPool.threadsNum(); i++) {
      // initialInstanceId used for fixed extra nonce part calculation
      unsigned initialInstanceId = instanceId*threadPool.threadsNum() + i;
      Data_[i].ThreadCfg.initialize(initialInstanceId, totalInstancesNum);
      Data_[i].WorkerBase = threadPool.getBase(i);

      // initialize work storage
      Data_[i].WorkStorage.init(linkedBackends, workResetBackends, primaryBackends, secondaryBackends);

      // initialize share accumulators and flush timer
      Data_[i].Accumulators.resize(linkedBackends.size());
      {
        Timestamp now = Timestamp::now();
        for (size_t j = 0; j < linkedBackends.size(); j++)
          Data_[i].Accumulators[j].initialize(linkedBackends[j]->getCoinInfo().WorkSummaryFlushInterval, now);
        if (AlgoMetaStatistic_)
          Data_[i].AlgoMetaAccumulator.initialize(AlgoMetaStatistic_->coinInfo().WorkSummaryFlushInterval, now, false);
      }
      Data_[i].FlushTimer = newUserEvent(Data_[i].WorkerBase, false, [](aioUserEvent*, void *arg) {
        static_cast<StratumInstance*>(arg)->flushWork();
      }, this);

      if (hasRtt) {
        Data_[i].Timer = newUserEvent(Data_[i].WorkerBase, false, [](aioUserEvent*, void *arg) {
          ((StratumInstance*)(arg))->rttTimerCb();
        }, this);
      }
    }

    HasRtt_ = hasRtt;
    ListenPort_ = port;
    X::Stratum::miningConfigInitialize(MiningCfg_, config);
  }

  virtual void start() override {
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++) {
      userEventStartTimer(Data_[i].FlushTimer, 1000000, 0);
      if (HasRtt_)
        userEventStartTimer(Data_[i].Timer, 1000000, 0);
    }

    createListener(MonitorBase_, ListenPort_, [](socketTy socket, HostAddress address, void *arg) {
      static_cast<StratumInstance*>(arg)->newFrontendConnection(socket, address);
    }, this);
  }

  virtual void checkNewBlockTemplate(CBlockTemplate *blockTemplate, PoolBackend *backend) override {
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, blockTemplate, backend));
    if (MiningStats_)
      MiningStats_->onWork(blockTemplate->Difficulty, backend);
  }

  virtual void stopWork() override {
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, nullptr, nullptr));
  }

  void buildBackendList(const std::vector<PoolBackend*> &linkedBackends,
                        std::optional<std::vector<std::string>> &backendNamesList,
                        const std::unordered_map<std::string, size_t> &backendsMap,
                        bool needCheck,
                        std::function<bool(const PoolBackend*)> fn,
                        std::vector<size_t> &result,
                        const char *listName) {
    result.clear();
    if (backendNamesList.has_value()) {
      for (const auto &name: backendNamesList.value()) {
        auto It = backendsMap.find(name);
        if (It == backendsMap.end()) {
          LOG_F(ERROR, "%s: unknown primary backend %s", Name_.c_str(), name.c_str());
          exit(1);
        }

        size_t index = It->second;
        if (needCheck && !fn(linkedBackends[index])) {
          LOG_F(ERROR, "%s: %s cannot be a part of %s", Name_.c_str(), name.c_str(), listName);
          exit(1);
        }

        result.push_back(index);
      }
    } else {
      for (size_t i = 0; i < linkedBackends.size(); i++) {
        if (fn(linkedBackends[i]))
          result.push_back(i);
      }
    }
  }

  void acceptConnection(unsigned workerId, socketTy socketFd, HostAddress address) {
    ThreadData &data = Data_[workerId];
    aioObject *socket = newSocketIo(data.WorkerBase, socketFd);
    Connection *connection = new Connection(this, socket, workerId, address);
    objectSetDestructorCb(aioObjectHandle(socket), [](aioObjectRoot*, void *arg) {
      delete static_cast<Connection*>(arg);
    }, connection);

    X::Stratum::workerConfigInitialize(connection->WorkerConfig, data.ThreadCfg);
    if (isDebugInstanceStratumConnections())
      LOG_F(1, "%s: new connection from %s", Name_.c_str(), connection->AddressHr.c_str());

    // Initialize share difficulty
    connection->setStratumDifficulty(ConstantShareDiff_);

    aioRead(connection->Socket, connection->Buffer, sizeof(connection->Buffer), afNone, 3000000, reinterpret_cast<aioCb*>(readCb), connection);
  }

  void acceptWork(CBlockTemplate *blockTemplate, PoolBackend *backend) {
    if (!blockTemplate) {
      return;
    }

    ThreadData &data = Data_[GetLocalThreadId()];
    typename X::Proto::AddressTy miningAddress;
    auto &backendConfig = backend->getConfig();
    auto &coinInfo = backend->getCoinInfo();
    const std::string &addr = backendConfig.MiningAddresses.get().MiningAddress;
    if (!X::Proto::decodeHumanReadableAddress(addr, coinInfo.PubkeyAddressPrefix, miningAddress)) {
      LOG_F(WARNING, "%s: mining address %s is invalid", coinInfo.Name.c_str(), addr.c_str());
      return;
    }

    std::vector<uint8_t> miningAddressData(miningAddress.begin(), miningAddress.end());
    data.WorkStorage.createWork(*blockTemplate, backend, coinInfo.Name, miningAddressData, backendConfig.CoinBaseMsg, MiningCfg_, Name_);

    // Select new work
    CWork *work = nullptr;
    if (ProfitSwitcherEnabled_) {
      // Search most profitable work
      auto calculateProfit = [this](CWork *work) -> double {
        double result = 0.0;
        for (size_t i = 0, ie = work->backendsNum(); i != ie; ++i) {
          PoolBackend *backend = work->backend(i);
          if (!backend)
            continue;

          result += work->getAbstractProfitValue(i, PriceFetcher_->getPrice(work->backendId(i)), backend->getProfitSwitchCoeff());
        }

        return result;
      };

      std::vector<std::pair<CWork*, double>> allWorks;
      for (size_t i = 0; i < LinkedBackends_.size(); i++) {
        if (!data.WorkStorage.isPrimaryBackend(i))
          continue;

        CWork *nextWork = data.WorkStorage.fetchWork(i);
        if (!nextWork)
          continue;

        allWorks.emplace_back(nextWork, calculateProfit(nextWork));
      }

      std::sort(allWorks.begin(), allWorks.end(), [](const auto &l, const auto &r) { return l.second < r.second; });
      if (!allWorks.empty() && allWorks.back().first != data.WorkStorage.currentWork()) {
        work = allWorks.back().first;

        std::string profitSwitcherInfo;
        for (auto I = allWorks.rbegin(), IE = allWorks.rend(); I != IE; ++I) {
          char buffer[64];
          snprintf(buffer, sizeof(buffer), "%.8lf", I->second);
          profitSwitcherInfo.push_back(' ');
          profitSwitcherInfo.append(workName(I->first));
          profitSwitcherInfo.push_back('(');
          profitSwitcherInfo.append(buffer);
          profitSwitcherInfo.push_back(')');
        }

        if (GetLocalThreadId() == 0)
          LOG_F(INFO, "[t=0] %s: ProfitSwitcher:%s", Name_.c_str(), profitSwitcherInfo.c_str());
      }
    } else {
      // Without profit switched we use main work source
      auto w = data.WorkStorage.fetchWork(MainWorkSource_);
      if (w != data.WorkStorage.currentWork())
        work = w;
    }

    if (work) {
      // Send work to all miners
      auto beginPt = std::chrono::steady_clock::now();

      // Flush accumulators for backends with new block
      for (size_t i = 0; i < LinkedBackends_.size(); i++) {
        if (data.WorkStorage.isNewBlock(i))
          flushAccumulator(i);
      }

      // Set block info (base reward + expected work) for accumulators from new work
      for (size_t i = 0; i < work->backendsNum(); i++)
        data.Accumulators[work->backendId(i)].setBlockInfo(work->baseBlockReward(i), work->expectedWork(i));

      // Calculate reset flag for new work & build notify message
      bool resetPreviousWork = data.WorkStorage.needSendResetSignal(work);
      work->buildNotifyMessage(resetPreviousWork);
      data.WorkStorage.setCurrentWork(work);

      int64_t currentTime = time(nullptr);
      unsigned counter = 0;
      for (auto &connection: data.Connections_) {
        connection->ResendCount = 0;
        stratumSendWork(connection, work, currentTime);
        counter++;
      }

      auto endPt = std::chrono::steady_clock::now();
      auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(endPt - beginPt).count();
      if (GetLocalThreadId() == 0)
        LOG_F(INFO, "[t=0] %s: Broadcast %s work %" PRIi64 "(reset=%s) & send to %u clients in %.3lf seconds", Name_.c_str(), workName(work).c_str(), work->stratumId(), resetPreviousWork ? "yes" : "no", counter, static_cast<double>(timeDiff)/1000.0);
    }
  }

private:
  class AcceptNewConnection : public CThreadPool::Task {
  public:
    AcceptNewConnection(StratumInstance &instance, socketTy socketFd, HostAddress address) : Instance_(instance), SocketFd_(socketFd), Address_(address) {}
    void run(unsigned workerId) final { Instance_.acceptConnection(workerId, SocketFd_, Address_); }
  private:
    StratumInstance &Instance_;
    socketTy SocketFd_;
    HostAddress Address_;
  };

  class AcceptWork : public CThreadPool::Task {
  public:
    AcceptWork(StratumInstance &instance, CBlockTemplate *blockTemplate, PoolBackend *backend) : Instance_(instance), BlockTemplate_(blockTemplate), Backend_(backend) {}
    void run(unsigned) final { Instance_.acceptWork(BlockTemplate_.get(), Backend_); }
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
    }

    void close() {
      if (Active) {
        deleteAioObject(Socket);
        Active = false;
      }
    }

    void setStratumDifficulty(double difficulty) {
      StratumDifficultyFp = difficulty;
      StratumDifficultyInt = X::Stratum::StratumMultiplier;
      StratumDifficultyInt.mulfp(difficulty);
      ShareTarget = X::Stratum::targetFromDifficulty(StratumDifficultyInt);
    }

    bool Initialized = false;
    StratumInstance *Instance;
    // Network
    aioObject *Socket;
    unsigned WorkerId;
    HostAddress Address;
    std::string AddressHr;
    unsigned SendCounter_ = 0;
    bool Active = true;
    bool IsCgMiner = false;
    bool IsNiceHash = false;
    int64_t LastUpdateTime = std::numeric_limits<int64_t>::max();
    // Stratum protocol decoding
    char Buffer[12288];
    size_t MsgTailSize = 0;
    // Mining info
    CWorkerConfig WorkerConfig;
    // Current share difficulty (one for all workers on connection)
    double StratumDifficultyFp;
    UInt<256> StratumDifficultyInt;
    UInt<256> ShareTarget;
    // Workers
    std::unordered_map<std::string, Worker> Workers;
    // Share statistic
    static constexpr unsigned SSWindowSize = 10;
    static constexpr unsigned SSWindowElementSize = 10;

    // TODO use cycle buffer here
    std::deque<uint32_t> InvalidShares;
    unsigned TotalSharesCounter = 0;
    unsigned InvalidSharesCounter = 0;
    unsigned InvalidSharesSequenceSize = 0;
    unsigned ResendCount = 0;
  };

  struct ThreadData {
    asyncBase *WorkerBase;
    ThreadConfig ThreadCfg;
    std::set<Connection*> Connections_;
    StratumWorkStorage<X> WorkStorage;
    aioUserEvent *Timer;
    std::vector<CShareAccumulator> Accumulators;
    CShareAccumulator AlgoMetaAccumulator;
    aioUserEvent *FlushTimer = nullptr;
  };

  void flushWork() {
    ThreadData &data = Data_[GetLocalThreadId()];
    Timestamp now = Timestamp::now();
    for (size_t i = 0; i < LinkedBackends_.size(); i++) {
      auto &acc = data.Accumulators[i];
      if (acc.shouldFlush(now)) {
        if (!acc.empty()) {
          auto batch = acc.takeBatch();
          LinkedBackends_[i]->sendWorkSummary(std::move(batch.Workers));
          LinkedBackends_[i]->sendUserWorkSummary(std::move(batch.Users));
        }
        acc.resetFlushTime(now);
      }
    }
    if (AlgoMetaStatistic_ && data.AlgoMetaAccumulator.shouldFlush(now)) {
      if (!data.AlgoMetaAccumulator.empty())
        AlgoMetaStatistic_->sendWorkSummary(std::move(data.AlgoMetaAccumulator.takeBatch().Workers));
      data.AlgoMetaAccumulator.resetFlushTime(now);
    }
  }

  void flushAccumulator(size_t globalBackendIdx) {
    ThreadData &data = Data_[GetLocalThreadId()];
    auto &acc = data.Accumulators[globalBackendIdx];
    if (!acc.empty()) {
      auto batch = acc.takeBatch();
      LinkedBackends_[globalBackendIdx]->sendWorkSummary(std::move(batch.Workers));
      LinkedBackends_[globalBackendIdx]->sendUserWorkSummary(std::move(batch.Users));
    }
    acc.resetFlushTime(Timestamp::now());
  }

private:
  void send(Connection *connection, const xmstream &stream) {
    if (isDebugInstanceStratumMessages()) {
      std::string msg(stream.data<char>(), stream.sizeOf());
      LOG_F(1, "%s(%s): outgoing message %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), msg.c_str());
    }
    if (++connection->SendCounter_ < UncheckedSendCount) {
      aioWrite(connection->Socket, stream.data(), stream.sizeOf(), afWaitAll, 0, nullptr, nullptr);
    } else {
      aioWrite(connection->Socket, stream.data(), stream.sizeOf(), afWaitAll, SendTimeout, [](AsyncOpStatus status, aioObject*, size_t, void *arg) {
        if (status != aosSuccess) {
          Connection *connection = static_cast<Connection*>(arg);
          LOG_F(1, "%s: send timeout to %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str());
          connection->close();
        }
      }, connection);
      connection->SendCounter_ = 0;
    }
  }

  void onStratumSubscribe(Connection *connection, CStratumMessage &msg) {
    if (msg.Subscribe.minerUserAgent.find("cgminer") != std::string::npos)
      connection->IsCgMiner = true;
    if (msg.Subscribe.minerUserAgent.find("NiceHash") != std::string::npos) {
      connection->IsNiceHash = true;
      if (AlgoMetaStatistic_->coinInfo().Name == "sha256") {
        connection->setStratumDifficulty(std::max(connection->StratumDifficultyFp, 500000.0));
      } else if (AlgoMetaStatistic_->coinInfo().Name == "scrypt") {
        connection->setStratumDifficulty(std::max(connection->StratumDifficultyFp, 655360.0));
      } else if (AlgoMetaStatistic_->coinInfo().Name == "equihash.200.9") {
        // TODO: check nicehash x11 threshold
        connection->setStratumDifficulty(std::max(connection->StratumDifficultyFp, 131072.0));
      } else if (AlgoMetaStatistic_->coinInfo().Name == "x11") {
        // TODO: check nicehash x11 threshold
        connection->setStratumDifficulty(std::max(connection->StratumDifficultyFp, 256.0));
      }
    }

    std::string subscribeInfo;
    char buffer[4096];
    xmstream stream(buffer, sizeof(buffer));
    stream.reset();
    X::Stratum::workerConfigOnSubscribe(connection->WorkerConfig, MiningCfg_, msg, stream, subscribeInfo);
    send(connection, stream);
    if (isDebugInstanceStratumConnections())
      LOG_F(1, "%s(%s): subscribe data: %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), subscribeInfo.c_str());
  }

  bool onStratumAuthorize(Connection *connection, CStratumMessage &msg) {
    bool authSuccess = false;
    std::string error;
    size_t dotPos = msg.Authorize.login.find('.');
    if (dotPos != msg.Authorize.login.npos) {
      Worker worker;
      worker.User.assign(msg.Authorize.login.begin(), msg.Authorize.login.begin() + dotPos);
      worker.WorkerName.assign(msg.Authorize.login.begin() + dotPos + 1, msg.Authorize.login.end());
      connection->Workers.insert(std::make_pair(msg.Authorize.login, worker));
      authSuccess = UserMgr_.checkUser(worker.User);
    } else {
      error = "Invalid user name format (username.workername required)";
    }

    char buffer[4096];
    xmstream stream(buffer, sizeof(buffer));
    stream.reset();
    {
      JSON::Object object(stream);
      msg.addId(object);
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

  bool onStratumMiningConfigure(Connection *connection, CStratumMessage &msg) {
    xmstream stream;
    std::string error;
    {
      JSON::Object object(stream);
      msg.addId(object);
      object.addField("result");
      {
        JSON::Object result(stream);
        if (msg.MiningConfigure.ExtensionsField & StratumMiningConfigure::EVersionRolling) {
          bool versionRolling = false;
          // Workaround about ASICs that not send "version-rolling.min-bit-count"
          if (msg.MiningConfigure.VersionRollingMask.has_value() && !msg.MiningConfigure.VersionRollingMinBitCount.has_value())
            msg.MiningConfigure.VersionRollingMinBitCount = 2;
          if (msg.MiningConfigure.VersionRollingMask.has_value() && msg.MiningConfigure.VersionRollingMinBitCount.has_value()) {
            // Calculate target bit mask
            uint32_t targetBitMask = msg.MiningConfigure.VersionRollingMask.value() & VersionMask_;
            // Get target bit count
            // TODO: use std::popcount since C++20
            unsigned count = popcount(targetBitMask);
            if (count >= msg.MiningConfigure.VersionRollingMinBitCount.value()) {
              result.addString("version-rolling.mask", writeHexBE(targetBitMask, 4));
              // TODO: write error if asic boost not supported
              X::Stratum::workerConfigSetupVersionRolling(connection->WorkerConfig, targetBitMask);
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
                  msg.MiningConfigure.VersionRollingMask.has_value() ? msg.MiningConfigure.VersionRollingMask.value() : 0,
                  msg.MiningConfigure.VersionRollingMinBitCount.has_value() ? msg.MiningConfigure.VersionRollingMinBitCount.value() : 0,
                  VersionMask_);
          }
          result.addBoolean("version-rolling", versionRolling);
        }

        if (msg.MiningConfigure.ExtensionsField & StratumMiningConfigure::EMinimumDifficulty) {
          bool minimumDifficulty = false;
          result.addBoolean("minimum-difficulty", minimumDifficulty);
        }

        if (msg.MiningConfigure.ExtensionsField & StratumMiningConfigure::ESubscribeExtraNonce) {
          bool subscribeExtraNonce = false;
          result.addBoolean("subscribe-extranonce", subscribeExtraNonce);
        }
      }

      object.addNull("error");
    }

    stream.write('\n');
    send(connection, stream);

    ThreadData &data = Data_[GetLocalThreadId()];
    CWork *currentWork = data.WorkStorage.currentWork();
    if (connection->IsCgMiner && currentWork)
      stratumSendWork(connection, currentWork, time(nullptr));
    return true;
  }

  void onStratumMiningSuggestDifficulty(Connection*, CStratumMessage&) {
    // Nothing to do
  }

  void onStratumExtraNonceSubscribe(Connection *connection, CStratumMessage &message) {
    xmstream stream;
    {
      JSON::Object object(stream);
//      addId(object, message);
      message.addId(object);
      object.addBoolean("result", true);
      object.addNull("error");
    }

    stream.write('\n');
    send(connection, stream);
  }

  void sharePendingCheck(size_t globalBackendIdx,
                         std::string userName,
                         int64_t majorJobId,
                         const CWorkerConfig &workerConfig,
                         CStratumMessage msg) {
    ThreadData &data = Data_[GetLocalThreadId()];
    CWork *work = data.WorkStorage.workById(majorJobId);
    if (!work)
      return;
    if (!work->prepareForSubmit(workerConfig, msg))
      return;

    // Find work-internal index matching this backend
    size_t workInternalBackendIdx = work->backendsNum();
    for (size_t i = 0, ie = work->backendsNum(); i != ie; ++i) {
      if (work->backendId(i) == globalBackendIdx) {
        workInternalBackendIdx = i;
        break;
      }
    }

    PoolBackend *backend = workInternalBackendIdx < work->backendsNum() ? work->backend(workInternalBackendIdx) : nullptr;
    if (!backend || !work->hasRtt(workInternalBackendIdx))
      return;

    // Check for block only, share target not need here
    CCheckStatus checkStatus = work->checkConsensus(workInternalBackendIdx, UInt<256>::zero());
    if (checkStatus.IsBlock) {
      auto shareHash = work->shareHash();
      std::vector<PoolBackend*> shareBackends;
      for (size_t i = 0, ie = work->backendsNum(); i != ie; ++i) {
        PoolBackend *b = work->backend(i);
        if (b && work->backendId(i) != globalBackendIdx)
          shareBackends.push_back(b);
      }

      std::string blockHash = work->blockHash(workInternalBackendIdx);
      LOG_F(INFO, "%s: pending proof of work %s sending; hash: %s; transactions: %zu", Name_.c_str(), backend->getCoinInfo().Name.c_str(), blockHash.c_str(), work->txNum(workInternalBackendIdx));

      // Serialize block
      xmstream blockHexData;
      work->buildBlock(workInternalBackendIdx, blockHexData);
      uint64_t height = work->height(workInternalBackendIdx);
      UInt<256> expectedWork = work->expectedWork(workInternalBackendIdx);
      UInt<384> generatedCoins = work->blockReward(workInternalBackendIdx);
      CNetworkClientDispatcher &dispatcher = backend->getClientDispatcher();
      dispatcher.aioSubmitBlock(data.WorkerBase, blockHexData.data(), blockHexData.sizeOf(),
        [height, blockHash, generatedCoins, expectedWork, globalBackendIdx, backend, this, userName, shareHash,
         shareBackends = std::move(shareBackends)](bool success, uint32_t successNum, const std::string &hostName, const std::string &error) {
        if (success) {
          LOG_F(INFO, "* block %s (%" PRIu64 ") accepted by %s", blockHash.c_str(), height, hostName.c_str());
          if (successNum == 1) {
            flushAccumulator(globalBackendIdx);
            CBlockFoundData *block = new CBlockFoundData;
            block->UserId = userName;
            block->Height = height;
            block->Hash = blockHash;
            block->Time = Timestamp::now();
            block->GeneratedCoins = generatedCoins;
            block->ExpectedWork = expectedWork;
            block->PrimePOWTarget = 0;
            backend->sendBlockFound(block, shareHash, std::move(shareBackends));
          }
        } else {
          LOG_F(ERROR, "* block %s (%" PRIu64 ") rejected by %s error: %s", blockHash.c_str(), height, hostName.c_str(), error.c_str());
        }
      });
    }
  }

  bool shareCheck(Connection *connection, CStratumMessage &msg, StratumErrorTy &errorCode) {
    ThreadData &data = Data_[GetLocalThreadId()];
    uint64_t height = 0;
    CCheckStatus checkStatus;
    std::string blockHash;
    std::vector<bool> foundBlockMask(LinkedBackends_.size(), false);

    // Check worker name
    auto It = connection->Workers.find(msg.Submit.WorkerName);
    if (It == connection->Workers.end()) {
      if (isDebugInstanceStratumRejects())
        LOG_F(1, "%s(%s) reject: unknown worker name: %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), msg.Submit.WorkerName.c_str());
      errorCode = StratumErrorUnauthorizedWorker;
      return false;
    }
    Worker &worker = It->second;

    // Check job id
    CWork *work = nullptr;

    {
      size_t sharpPos = msg.Submit.JobId.find('#');
      if (sharpPos == msg.Submit.JobId.npos) {
        if (isDebugInstanceStratumRejects())
          LOG_F(1, "%s(%s) %s/%s reject: invalid job id format: %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), worker.User.c_str(), worker.WorkerName.c_str(), msg.Submit.JobId.c_str());
        errorCode = StratumErrorJobNotFound;
        return false;
      }

      msg.Submit.JobId[sharpPos] = 0;
      int64_t majorJobId = xatoi<uint64_t>(msg.Submit.JobId.c_str());
      work = data.WorkStorage.workById(majorJobId);
      if (!work) {
        if (isDebugInstanceStratumRejects())
          LOG_F(1, "%s(%s) %s/%s reject: unknown job id: %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), worker.User.c_str(), worker.WorkerName.c_str(), msg.Submit.JobId.c_str());
        errorCode = StratumErrorJobNotFound;
        return false;
      }
    }

    // Build header and check proof of work
    std::string user = worker.User;
    if (!work->prepareForSubmit(connection->WorkerConfig, msg)) {
      if (isDebugInstanceStratumRejects())
        LOG_F(1, "%s(%s) %s/%s reject: invalid share format", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), worker.User.c_str(), worker.WorkerName.c_str());
      errorCode = StratumErrorInvalidShare;
      return false;
    }

    // Check for duplicate
    typename X::Proto::BlockHashTy shareHash = work->shareHash();
    if (data.WorkStorage.isDuplicate(work, shareHash)) {
      if (isDebugInstanceStratumRejects())
        LOG_F(1, "%s(%s) %s/%s reject: duplicate share", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), worker.User.c_str(), worker.WorkerName.c_str());
      errorCode = StratumErrorDuplicateShare;
      return false;
    }

    // Loop over all affected backends (usually 1 or 2 with enabled merged mining)
    bool shareAccepted = false;
    for (size_t i = 0, ie = work->backendsNum(); i != ie; ++i) {
      PoolBackend *backend = work->backend(i);
      if (!backend) {
        if (isDebugInstanceStratumRejects())
          LOG_F(1, "%s(%s) %s/%s sub-reject: backend %zu not initialized", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), worker.User.c_str(), worker.WorkerName.c_str(), i);
        continue;
      }

      size_t globalBackendIdx = work->backendId(i);
      blockHash = work->blockHash(i);

      height = work->height(i);
      checkStatus = work->checkConsensus(i, connection->ShareTarget);

      if (!checkStatus.IsShare) {
        if (isDebugInstanceStratumRejects())
          LOG_F(1,
                "%s(%s) %s/%s sub-reject(%s): invalid share (below target)",
                connection->Instance->Name_.c_str(),
                connection->AddressHr.c_str(),
                worker.User.c_str(),
                worker.WorkerName.c_str(),
                backend->getCoinInfo().Name.c_str());
        continue;
      }

      shareAccepted = true;
      data.Accumulators[globalBackendIdx].addShare(worker.User, worker.WorkerName,
        connection->StratumDifficultyInt, Timestamp::now(), 0, 0, false);

      if (checkStatus.IsBlock) {
        LOG_F(INFO, "%s: new proof of work for %s found; hash: %s; transactions: %zu", Name_.c_str(), backend->getCoinInfo().Name.c_str(), blockHash.c_str(), work->txNum(i));

        // Serialize block
        xmstream blockHexData;
        work->buildBlock(i, blockHexData);

        std::vector<PoolBackend*> shareBackends;
        for (size_t j = 0; j != ie; ++j) {
          PoolBackend *b = work->backend(j);
          if (b && work->backendId(j) != globalBackendIdx)
            shareBackends.push_back(b);
        }

        UInt<384> generatedCoins = work->blockReward(i);
        UInt<256> expectedWork = work->expectedWork(i);
        CNetworkClientDispatcher &dispatcher = backend->getClientDispatcher();
        dispatcher.aioSubmitBlock(data.WorkerBase, blockHexData.data(), blockHexData.sizeOf(),
          [height, blockHash, generatedCoins, expectedWork, globalBackendIdx, backend, this, user=worker.User, shareHash,
           shareBackends = std::move(shareBackends)](bool success, uint32_t successNum, const std::string &hostName, const std::string &error) {
          if (success) {
            LOG_F(INFO, "* block %s (%" PRIu64 ") accepted by %s", blockHash.c_str(), height, hostName.c_str());
            if (successNum == 1) {
              flushAccumulator(globalBackendIdx);
              CBlockFoundData *block = new CBlockFoundData;
              block->UserId = user;
              block->Height = height;
              block->Hash = blockHash;
              block->Time = Timestamp::now();
              block->GeneratedCoins = generatedCoins;
              block->ExpectedWork = expectedWork;
              block->PrimePOWTarget = 0;
              backend->sendBlockFound(block, shareHash, std::move(shareBackends));
            }
          } else {
            LOG_F(ERROR, "* block %s (%" PRIu64 ") rejected by %s error: %s", blockHash.c_str(), height, hostName.c_str(), error.c_str());
          }
        });

        foundBlockMask[globalBackendIdx] = true;
      } else if (checkStatus.IsPendingBlock) {
        if (data.WorkStorage.updatePending(globalBackendIdx, worker.User, worker.WorkerName, checkStatus.Hash, connection->StratumDifficultyInt, xatoi<uint64_t>(msg.Submit.JobId.c_str()), connection->WorkerConfig, msg))
          LOG_F(INFO, "%s: new pending block %s found; hash: %s", Name_.c_str(), backend->getCoinInfo().Name.c_str(), blockHash.c_str());
      }
    }

    if (shareAccepted && AlgoMetaStatistic_) {
      data.AlgoMetaAccumulator.addShare(worker.User, worker.WorkerName,
        connection->StratumDifficultyInt, Timestamp::now(), 0, 0, false);
    }

    if (!shareAccepted)
      errorCode = StratumErrorInvalidShare;

    // TEMPORARY DISABLED
    if (shareAccepted && MiningStats_)
      MiningStats_->onShare(0.0, 0.0, LinkedBackends_, foundBlockMask, BaseBlob<256>::zero());

    return shareAccepted;
  }

  void onStratumSubmit(Connection *connection, CStratumMessage &msg) {
    StratumErrorTy errorCode;
    bool result = shareCheck(connection, msg, errorCode);

    // Update invalid shares statistic
    connection->TotalSharesCounter++;
    if (!result) {
      connection->InvalidSharesCounter++;
      connection->InvalidSharesSequenceSize++;
    } else {
      connection->InvalidSharesSequenceSize = 0;
    }

    if (connection->TotalSharesCounter == Connection::SSWindowElementSize) {
      connection->InvalidShares.push_back(connection->InvalidSharesCounter);
      connection->InvalidSharesCounter = 0;
      connection->TotalSharesCounter = 0;
      if (connection->InvalidShares.size() > Connection::SSWindowSize)
        connection->InvalidShares.pop_front();
    }

    xmstream stream;
    {
      // Response
      JSON::Object object(stream);
      msg.addId(object);
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
      // Calculate invalid shares percentage
      uint32_t invalidSharedPercent = 0;
      if (connection->InvalidShares.size() >= 3) {
        uint32_t invalidShares = 0;
        for (auto v: connection->InvalidShares)
          invalidShares += v;
        invalidSharedPercent = invalidShares * 100 / (connection->InvalidShares.size()*Connection::SSWindowElementSize);
      }

      if (invalidSharedPercent >= 20) {
        if (isDebugInstanceStratumConnections())
          LOG_F(1, "%s: connection %s: too much errors, disconnecting...", Name_.c_str(), connection->AddressHr.c_str());
        connection->close();
        return;
      }

      // Change job for duplicate/invalid share (handle unstable clients)
      // Do it every 4 sequential invalid shares
      if ((errorCode == StratumErrorDuplicateShare || errorCode == StratumErrorInvalidShare) && (connection->InvalidSharesSequenceSize % 4 == 0)) {
        ThreadData &data = Data_[GetLocalThreadId()];
        // "Mutate" newest available work
        CWork *work = data.WorkStorage.currentWork();
        if (work) {
          work->mutate();
          connection->ResendCount++;
          stratumSendWork(connection, work, time(nullptr));
        }
      }
    }
  }

  void onStratumMultiVersion(Connection *connection, CStratumMessage &message) {
    xmstream stream;
    {
      JSON::Object object(stream);
      message.addId(object);
      object.addBoolean("result", true);
      object.addNull("error");
    }

    stream.write('\n');
    send(connection, stream);
  }

  void onStratumSubmitHashrate(Connection *connection, CStratumMessage &message) {
    xmstream stream;
    {
      JSON::Object object(stream);
      message.addId(object);
      object.addBoolean("result", true);
      object.addNull("error");
    }

    stream.write('\n');
    send(connection, stream);
  }

  void stratumSendTarget(Connection *connection) {
    xmstream stream;
    X::Stratum::buildSendTargetMessage(stream, connection->StratumDifficultyFp);
    stream.write('\n');
    send(connection, stream);
  }

  void stratumSendWork(Connection *connection, CWork *work, int64_t currentTime) {
    connection->LastUpdateTime = currentTime;
    send(connection, work->notifyMessage());
  }

  void newFrontendConnection(socketTy fd, HostAddress address) {
    // Functions runs inside 'monitor' thread
    // Send task to one of worker threads
    ThreadPool_.startAsyncTask(CurrentThreadId_, new AcceptNewConnection(*this, fd, address));
    CurrentThreadId_ = (CurrentThreadId_ + 1) % ThreadPool_.threadsNum();
  }

  static void readCb(AsyncOpStatus status, aioObject*, size_t size, Connection *connection) {
    if (status != aosSuccess) {
      connection->close();
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
      CStratumMessage msg;
      size_t stratumMsgSize = nextMsgPos - p;
      if (isDebugInstanceStratumMessages()) {
        std::string msg(p, stratumMsgSize);
        LOG_F(1, "%s(%s): incoming message %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), msg.c_str());
      }

      switch (X::Stratum::decodeStratumMessage(msg, p, stratumMsgSize)) {
        case EStratumDecodeStatusTy::EStratumStatusOk :
          // Process stratum messages here
          switch (msg.Method) {
            case EStratumMethodTy::ESubscribe :
              connection->Instance->onStratumSubscribe(connection, msg);
              break;
            case EStratumMethodTy::EAuthorize : {
              result = connection->Instance->onStratumAuthorize(connection, msg);
              connection->Instance->stratumSendTarget(connection);
              CWork *currentWork = data.WorkStorage.currentWork();
              if (result && currentWork) {
                connection->Instance->stratumSendWork(connection, currentWork, time(nullptr));
              }
              break;
            }
            case EStratumMethodTy::EMiningConfigure :
              connection->Instance->onStratumMiningConfigure(connection, msg);
              break;
            case EStratumMethodTy::EMiningSuggestDifficulty :
              connection->Instance->onStratumMiningSuggestDifficulty(connection, msg);
              break;
            case EStratumMethodTy::EExtraNonceSubscribe :
              connection->Instance->onStratumExtraNonceSubscribe(connection, msg);
              break;
            case EStratumMethodTy::ESubmit :
              connection->Instance->onStratumSubmit(connection, msg);
              break;
            case EStratumMethodTy::EMultiVersion :
              connection->Instance->onStratumMultiVersion(connection, msg);
              break;
            case EStratumMethodTy::ESubmitHashrate :
              connection->Instance->onStratumSubmitHashrate(connection, msg);
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
        case EStratumDecodeStatusTy::EStratumStatusJsonError : {
          std::string msg(p, stratumMsgSize);
          if (isDebugInstanceStratumMessages())
            LOG_F(1, "%s(%s): JsonError %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), msg.c_str());
          result = false;
          break;
        }
        case EStratumDecodeStatusTy::EStratumStatusFormatError : {
          std::string msg(p, stratumMsgSize);
          if (isDebugInstanceStratumMessages())
            LOG_F(1, "%s(%s): FormatError %s", connection->Instance->Name_.c_str(), connection->AddressHr.c_str(), msg.c_str());
          result = false;
          break;
        }
        default :
          break;
      }

      if (!result) {
        connection->close();
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
        connection->close();
        return;
      }

      memmove(connection->Buffer, p, e-p);
    } else {
      connection->MsgTailSize = 0;
    }

    if (connection->Active)
      aioRead(connection->Socket, connection->Buffer + connection->MsgTailSize, sizeof(connection->Buffer) - connection->MsgTailSize, afNone, 0, reinterpret_cast<aioCb*>(readCb), connection);
  }

  void rttTimerCb() {
    ThreadData &data = Data_[GetLocalThreadId()];
    size_t backendsNum = data.WorkStorage.backendsNum();
    for (size_t i = 0; i < backendsNum; i++) {
      const auto &share = data.WorkStorage.pendingShare(i);
      if (share.HasShare) {
        sharePendingCheck(i,
                          share.User,
                          share.MajorJobId,
                          share.WorkerConfig,
                          share.Msg);
      }
    }
  }

  std::string workName(CWork *work) {
    std::string name;
    for (size_t i = 0; i < work->backendsNum(); i++) {
      if (work->backend(i)) {
        if (!name.empty())
          name.push_back('+');
        name.append(work->backend(i)->getCoinInfo().Name);
      }
    }

    return name;
  }

private:
  unsigned CurrentThreadId_;
  std::unique_ptr<ThreadData[]> Data_;
  std::string Name_ = "stratum";
  CMiningConfig MiningCfg_;
  double ConstantShareDiff_;

  // Profit switcher section
  bool ProfitSwitcherEnabled_ = false;
  size_t MainWorkSource_;

  // ASIC boost 'overt' data
  uint32_t VersionMask_ = 0x1FFFE000;

  bool HasRtt_ = false;
  uint16_t ListenPort_ = 0;

  // Price fetcher
  CPriceFetcher *PriceFetcher_ = nullptr;
};
