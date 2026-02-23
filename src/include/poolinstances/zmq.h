#pragma once

#include "common.h"
#include "poolcore/backend.h"
#include "poolcore/poolCore.h"
#include "poolcore/poolInstance.h"
#include "poolcore/thread.h"
#include "poolcore/usermgr.h"
#include "poolcore/shareAccumulator.h"
#include "blockmaker/merkleTree.h"
#include <asyncio/socket.h>
#include <asyncioextras/zmtp.h>
#include "blockmaker/protocol.pb.h"

static constexpr unsigned REQUIRED_MINER_VERSION = 1061;

static bool checkRequest(pool::proto::Request &req,
                         pool::proto::Reply &rep,
                         void *msg,
                         size_t msgSize)
{
  // TODO: size limit check
  if (!req.ParseFromArray(msg, static_cast<int>(msgSize))) {
    LOG_F(WARNING, "invalid message received");
    return false;
  }

  rep.Clear();
  rep.set_type(req.type());
  rep.set_reqid(req.reqid());
  rep.set_error(pool::proto::Reply::NONE);
  return true;
}

template<typename X>
class ZmqInstance : public CPoolInstance {
public:
  ZmqInstance(asyncBase *monitorBase,
              UserManager &userMgr,
              const std::vector<PoolBackend*> &linkedBackends,
              CThreadPool &threadPool,
              StatisticServer *algoMetaStatistic,
              ComplexMiningStats *miningStats,
              unsigned instanceId,
              unsigned instancesNum,
              rapidjson::Value &config)
      : CPoolInstance(monitorBase, userMgr, linkedBackends, threadPool, algoMetaStatistic, miningStats)
  {
    Name_ = (std::string)X::Proto::TickerName + ".zmq";
    Data_.reset(new ThreadData[threadPool.threadsNum()]);
    X::Zmq::initialize();

    for (size_t i = 0; i < linkedBackends.size(); i++)
      BackendMap_[linkedBackends[i]] = i;

    unsigned totalInstancesNum = instancesNum * threadPool.threadsNum();
    for (unsigned i = 0; i < threadPool.threadsNum(); i++) {
      // initialInstanceId used for fixed extra nonce part calculation
      unsigned initialInstanceId = instanceId*threadPool.threadsNum() + i;
      X::Zmq::initializeThreadConfig(Data_[i].ThreadCfg, initialInstanceId, totalInstancesNum);

      Data_[i].WorkerBase = threadPool.getBase(i);
      Data_[i].HasWork = false;
      X::Proto::checkConsensusInitialize(Data_[i].CheckConsensusCtx);

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
        static_cast<ZmqInstance*>(arg)->flushWork();
      }, this);
    }

    if (!(config.HasMember("port") && config["port"].IsInt() &&
          config.HasMember("workerPort") && config["workerPort"].IsInt() &&
          config.HasMember("hostName") && config["hostName"].IsString())) {
      LOG_F(ERROR, "instance %s: can't read 'port', 'workerPort', 'hostName' values from config", "XPM/zmq");
      exit(1);
    }

    ListenPort_ = config["port"].GetInt();
    WorkerPort_ = config["workerPort"].GetInt();
    HostName_ = config["hostName"].GetString();

    X::Zmq::initializeMiningConfig(MiningCfg_, config);
  }

  virtual void start() override {
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      userEventStartTimer(Data_[i].FlushTimer, 1000000, 0);

    // Frontend listener
    createListener(MonitorBase_, ListenPort_, [](socketTy socket, HostAddress, void *arg) {
      static_cast<ZmqInstance*>(arg)->newFrontendConnection(socket);
    }, this);

    // Worker/signal listeners (2*<worker num> ports used)
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++) {
      createListener(
        Data_[i].WorkerBase,
        WorkerPort_ + i*2,
        [](socketTy socket, HostAddress address, void *arg) {
          static_cast<ZmqInstance*>(arg)->newWorkerConnection(socket, address);
        },
        this);
      createListener(
        Data_[i].WorkerBase,
        WorkerPort_ + i*2 + 1,
        [](socketTy socket, HostAddress, void *arg) {
          static_cast<ZmqInstance*>(arg)->newSignalsConnection(socket);
        },
        this);
    }
  }

  virtual void checkNewBlockTemplate(CBlockTemplate *blockTemplate, PoolBackend *backend) override {
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, blockTemplate, backend));
  }

  virtual void stopWork() override {
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, nullptr, nullptr));
  }

private:
  class AcceptWork : public CThreadPool::Task {
  public:
    AcceptWork(ZmqInstance &instance, CBlockTemplate *blockTemplate, PoolBackend *backend) : Instance_(instance), BlockTemplate_(blockTemplate), Backend_(backend) {}
    void run(unsigned workerId) final { Instance_.acceptWork(workerId, BlockTemplate_.get(), Backend_); }
  private:
    ZmqInstance &Instance_;
    intrusive_ptr<CBlockTemplate> BlockTemplate_;
    PoolBackend *Backend_;
  };

  struct Connection {
    Connection(ZmqInstance *instance, zmtpSocket *socket, unsigned workerId, bool isSignal) : Instance(instance), Socket(socket), WorkerId(workerId), IsSignal(isSignal), IsConnected(true) {
      if (isSignal)
        instance->Data_[workerId].SignalSockets.insert(this);
    }

    ~Connection() {
      if (IsSignal)
        Instance->Data_[WorkerId].SignalSockets.erase(this);
      zmtpSocketDelete(Socket);
    }

    ZmqInstance *Instance;
    zmtpSocket *Socket;
    unsigned WorkerId;
    bool IsSignal;
    bool IsConnected;
    zmtpStream Stream;
    zmtpUserMsgTy MsgType;
    typename X::Zmq::WorkerConfig WorkerConfig;
  };

  struct ThreadData {
    asyncBase *WorkerBase;
    typename X::Proto::CheckConsensusCtx CheckConsensusCtx;
    typename X::Zmq::ThreadConfig ThreadCfg;
    typename X::Zmq::Work Work;
    typename X::Zmq::ThreadConfig ThreadConfig;
    bool HasWork;
    std::set<Connection*> SignalSockets;
    std::unordered_set<BaseBlob<256>> KnownShares;
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
  void sendWork(ThreadData &data, Connection *connection) {
    X::Zmq::generateNewWork(data.Work, connection->WorkerConfig, data.ThreadConfig, MiningCfg_);

    // Fill block protobuf
    pool::proto::Signal sig;
    sig.set_type(pool::proto::Signal::NEWBLOCK);
    X::Zmq::buildBlockProto(data.Work, MiningCfg_, *sig.mutable_block());
    X::Zmq::buildWorkProto(data.Work, *sig.mutable_work());

    size_t repSize = sig.ByteSizeLong();
    connection->Stream.reset();
    connection->Stream.template write<uint8_t>(1);
    // TODO: size limit check
    sig.SerializeToArray(connection->Stream.reserve(repSize), static_cast<int>(repSize));
    aioZmtpSend(connection->Socket, connection->Stream.data(), connection->Stream.sizeOf(), zmtpMessage, afNone, 0, [](AsyncOpStatus status, zmtpSocket*, void *arg) {
      if (status != aosSuccess)
        static_cast<Connection*>(arg)->IsConnected = false;
    }, connection);
  }

  void onSetConfig(ThreadData&, Connection *connection, pool::proto::Request &req, pool::proto::Reply &rep) {
    if (!req.has_weavedepth()) {
      rep.set_errstr("Client sent empty config");
      return;
    }

    if (!connection->WorkerConfig.setConfig(req.weavedepth())) {
      rep.set_errstr("Client sent invalid config");
      return;
    }
  }

  void onGetWork(ThreadData &data, Connection *connection, pool::proto::Request&, pool::proto::Reply &rep) {
    if (data.HasWork) {
      X::Zmq::generateNewWork(data.Work, connection->WorkerConfig, data.ThreadConfig, MiningCfg_);
      X::Zmq::buildWorkProto(data.Work, *rep.mutable_work());
    } else {
      std::string error = Name_ + ": no network connection";
      rep.set_error(pool::proto::Reply::HEIGHT);
      rep.set_errstr(error);
    }
  }

  void onShare(Connection *connection, ThreadData &data, pool::proto::Request &req, pool::proto::Reply &rep) {
    typename X::Zmq::Work &work = data.Work;
    uint64_t height = work.height();
    PoolBackend *backend = work.backend();
    std::vector<bool> foundBlockMask(LinkedBackends_.size(), false);

    if (!data.HasWork) {
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }

    // share existing
    if (!req.has_share()) {
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }

    const pool::proto::Share& share = req.share();

    if (!connection->WorkerConfig.hasConfig()) {
      rep.set_errstr("Mining config not set");
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }

    // check user name
    // TODO: move it to connection
    UserManager::Credentials credentials;
    if (!UserMgr_.getUserCredentials(share.addr(), credentials)) {
      rep.set_errstr((std::string)"Unknown user " + share.addr());
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }

    if (share.name().empty()) {
      rep.set_errstr("Worker can't be empty");
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }

    // block height
    if (share.height() != height) {
      rep.set_error(pool::proto::Reply::STALE);
      return;
    }

    // candidate type must be valid
    if (share.chaintype() > 2) {
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }

    // minimal share length
    if (share.length() < MiningCfg_.MinShareLength) {
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }

    if (!X::Zmq::prepareToSubmit(work, data.ThreadConfig, MiningCfg_, req, rep))
      return;

    // check duplicate
    typename X::Proto::BlockHashTy blockHash = work.hash();
    if (!data.KnownShares.insert(blockHash).second) {
      rep.set_error(pool::proto::Reply::DUPLICATED);
      return;
    }

    // check proof of work
    std::string workerId = share.name();
    double shareDiff = 0.0;
    UInt<256> shareWork = UInt<256>::zero();
    uint32_t primePOWTarget = 0;

    typename X::Zmq::Work::CExtraInfo info;
    bool isBlock = work.checkConsensus(data.CheckConsensusCtx, &shareDiff, &info);
    if (shareDiff < MiningCfg_.MinShareLength) {
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }

    primePOWTarget = std::max(work.primePOWTarget(), MiningCfg_.MinShareLength);
    shareWork = work.shareWork(data.CheckConsensusCtx, shareDiff, MiningCfg_.MinShareLength, info, connection->WorkerConfig);
    if (shareWork.isZero()) {
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }

    size_t globalBackendIdx = work.backendId();
    bool isPrimePOW = backend->getCoinInfo().PowerUnitType == CCoinInfo::ECPD;
    data.Accumulators[globalBackendIdx].addShare(share.addr(), workerId,
      shareWork, Timestamp::now(), shareDiff, primePOWTarget, isPrimePOW);
    if (AlgoMetaStatistic_)
      data.AlgoMetaAccumulator.addShare(share.addr(), workerId,
        shareWork, Timestamp::now(), shareDiff, primePOWTarget, isPrimePOW);

    if (isBlock) {
      LOG_F(INFO, "%s: new proof of work found hash: %s transactions: %zu", Name_.c_str(), blockHash.getHexLE().c_str(), data.Work.txNum());
      // Submit to nodes
      UInt<256> expectedWork = work.expectedWork();

      std::string user = share.addr();
      UInt<384> generatedCoins = work.blockReward();
      CNetworkClientDispatcher &dispatcher = backend->getClientDispatcher();
      dispatcher.aioSubmitBlock(data.WorkerBase,
                                work.blockHexData().data(),
                                work.blockHexData().sizeOf(),
                                [height, user, blockHash, generatedCoins, expectedWork, primePOWTarget, globalBackendIdx, backend, this](bool success, uint32_t successNum, const std::string &hostName, const std::string &error) {
        if (success) {
          LOG_F(INFO, "* block %s (%" PRIu64 ") accepted by %s", blockHash.getHexLE().c_str(), height, hostName.c_str());
          if (successNum == 1) {
            flushAccumulator(globalBackendIdx);
            CBlockFoundData *block = new CBlockFoundData;
            block->UserId = user;
            block->Height = height;
            block->Hash = blockHash.getHexLE();
            block->Time = Timestamp::now();
            block->GeneratedCoins = generatedCoins;
            block->ExpectedWork = expectedWork;
            block->PrimePOWTarget = primePOWTarget;
            backend->sendBlockFound(block, {});
          }
        } else {
          LOG_F(ERROR, "* block %s (%" PRIu64 ") rejected by %s error: %s", blockHash.getHexLE().c_str(), height, hostName.c_str(), error.c_str());
        }
      });

      foundBlockMask[globalBackendIdx] = true;
    }

    // Disabled now
    // if (MiningStats_)
    //  MiningStats_->onShare(shareDiff, connection->ShareDifficulty, LinkedBackends_, foundBlockMask, shareHash);
  }

  void onStats(ThreadData &data, pool::proto::Request &req, pool::proto::Reply &rep) {
    if (!req.has_stats()) {
      LOG_F(WARNING, "!req.has_stats()");
      return;
    }

    PoolBackend *backend = data.Work.backend();
    if (!backend)
      return;

    const pool::proto::ClientStats &src = req.stats();

    // check user name
    // TODO: move it to connection
    UserManager::Credentials credentials;
    if (!UserMgr_.getUserCredentials(src.addr(), credentials)) {
      rep.set_errstr((std::string)"Unknown user " + src.addr());
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }
  }

  void newFrontendConnection(socketTy fd) {
    Connection *connection = new Connection(this, zmtpSocketNew(MonitorBase_, newSocketIo(MonitorBase_, fd), zmtpSocketDEALER), -1, false);
    aioZmtpAccept(connection->Socket, afNone, 5000000, [](AsyncOpStatus status, zmtpSocket*, void *arg) {
      Connection *connection = static_cast<Connection*>(arg);
      if (status == aosSuccess) {
        aioZmtpRecv(connection->Socket, connection->Stream, 65536, afNone, 0, frontendRecvCb, connection);
       } else {
        delete connection;
      }
    }, connection);
  }

  void newWorkerConnection(socketTy fd, HostAddress) {
    ThreadData &data = Data_[GetLocalThreadId()];
    Connection *connection = new Connection(this, zmtpSocketNew(data.WorkerBase, newSocketIo(data.WorkerBase, fd), zmtpSocketROUTER), GetLocalThreadId(), false);

    connection->WorkerConfig.initialize(data.ThreadCfg);

    aioZmtpAccept(connection->Socket, afNone, 5000000, [](AsyncOpStatus status, zmtpSocket*, void *arg) {
      Connection *connection = static_cast<Connection*>(arg);
      if (status == aosSuccess) {
        aioZmtpRecv(connection->Socket, connection->Stream, 65536, afNone, 5000000, workerRecvCb, connection);
      } else {
        delete connection;
      }
    }, connection);
  }

  void newSignalsConnection(socketTy fd) {
    ThreadData &data = Data_[GetLocalThreadId()];
    Connection *connection = new Connection(this, zmtpSocketNew(data.WorkerBase, newSocketIo(data.WorkerBase, fd), zmtpSocketPUB), GetLocalThreadId(), true);

    connection->WorkerConfig.initialize(data.ThreadCfg);

    aioZmtpAccept(connection->Socket, afNone, 5000000, [](AsyncOpStatus status, zmtpSocket*, void *arg) {
      Connection *connection = static_cast<Connection*>(arg);
      if (status == aosSuccess) {
        ThreadData &data = connection->Instance->Data_[GetLocalThreadId()];
        if (data.HasWork)
          connection->Instance->sendWork(data, connection);
      } else {
        delete connection;
      }
    }, connection);
  }

  void acceptWork(unsigned, CBlockTemplate *blockTemplate, PoolBackend *backend) {
    ThreadData &data = Data_[GetLocalThreadId()];
    if (!blockTemplate) {
      data.HasWork = false;
      return;
    }

    // Get mining address and coinbase message
    auto &backendConfig = backend->getConfig();
    auto &coinInfo = backend->getCoinInfo();
    typename X::Proto::AddressTy miningAddress;
    const std::string &addr = backendConfig.MiningAddresses.get().MiningAddress;
    if (!X::Proto::decodeHumanReadableAddress(addr, coinInfo.PubkeyAddressPrefix, miningAddress)) {
      LOG_F(WARNING, "%s: mining address %s is invalid", coinInfo.Name.c_str(), addr.c_str());
      return;
    }

    std::string error;
    if (!X::Zmq::loadFromTemplate(data.Work, blockTemplate->Document, MiningCfg_, backend, coinInfo.Name, miningAddress, backendConfig.CoinBaseMsg, error)) {
      LOG_F(ERROR, "%s: can't process block template; error: %s", Name_.c_str(), error.c_str());
      return;
    }

    data.Work.BackendId = BackendMap_[backend];

    data.HasWork = true;

    X::Zmq::resetThreadConfig(data.ThreadConfig);
    data.KnownShares.clear();

    // Cleanup
    std::vector<Connection*> disconnected;
    for (Connection *connection: data.SignalSockets) {
      if (!connection->IsConnected)
        disconnected.push_back(connection);
    }
    for (Connection *connection: disconnected)
      delete connection;

    // Flush accumulated shares before sending new work
    flushAccumulator(data.Work.BackendId);

    // Set block info (base reward + expected work) for accumulator from new work
    data.Accumulators[data.Work.BackendId].setBlockInfo(data.Work.baseBlockReward(), data.Work.expectedWork());

    // Send signals
    for (Connection *connection: data.SignalSockets)
      sendWork(data, connection);
  }

  static void frontendRecvCb(AsyncOpStatus status, zmtpSocket*, zmtpUserMsgTy type, zmtpStream*, void *arg) {
    Connection *connection = static_cast<Connection*>(arg);
    if (status == aosSuccess)
      connection->Instance->frontendProc(connection, type);
    else
      delete connection;
  }

  static void workerRecvCb(AsyncOpStatus status, zmtpSocket*, zmtpUserMsgTy type, zmtpStream*, void *arg) {
    Connection *connection = static_cast<Connection*>(arg);
    if (status == aosSuccess)
      connection->Instance->workerProc(connection, type);
    else
      delete connection;
  }

  void frontendProc(Connection *connection, zmtpUserMsgTy type) {
    pool::proto::Request req;
    pool::proto::Reply rep;
    if (type != zmtpMessage || !checkRequest(req, rep, connection->Stream.data(), connection->Stream.remaining())) {
      delete connection;
      return;
    }

    pool::proto::Request::Type requestType = req.type();
    if (requestType == pool::proto::Request::CONNECT) {
      if (!req.has_version() || req.version() < REQUIRED_MINER_VERSION) {
        char buffer[1024];
        snprintf(buffer,
                 sizeof(buffer),
                 "Miner not supported by pool, minimal version is %u.%u.%u",
                 REQUIRED_MINER_VERSION / 100,
                 (REQUIRED_MINER_VERSION % 100) / 10,
                 (REQUIRED_MINER_VERSION % 100) % 10);
        rep.set_error(pool::proto::Reply::VERSION);
        rep.set_errstr(buffer);
      }

      // fill 'bitcoin' and 'signals' host addresses
      pool::proto::ServerInfo mServerInfo;
      mServerInfo.set_host(HostName_);
      mServerInfo.set_router(WorkerPort_ + CurrentWorker_*2);
      mServerInfo.set_pub(WorkerPort_ + CurrentWorker_*2 + 1);
      // XPM specific
      // TODO: get from block template (nBits)
      mServerInfo.set_target(10);
      mServerInfo.set_versionmajor(REQUIRED_MINER_VERSION / 100);
      mServerInfo.set_versionminor(REQUIRED_MINER_VERSION % 100);
      rep.mutable_sinfo()->CopyFrom(mServerInfo);
      CurrentWorker_ = (CurrentWorker_ + 1) % ThreadPool_.threadsNum();
    }

    size_t repSize = rep.ByteSizeLong();
    connection->Stream.reset();
    // TODO: size limit check
    rep.SerializeToArray(connection->Stream.reserve(repSize), static_cast<int>(repSize));
    aioZmtpSend(connection->Socket, connection->Stream.data(), connection->Stream.sizeOf(), zmtpMessage, afNone, 0, nullptr, nullptr);
    aioZmtpRecv(connection->Socket, connection->Stream, 65536, afNone, 0, frontendRecvCb, connection);
  }

  void workerProc(Connection *connection, zmtpUserMsgTy type) {
    pool::proto::Request req;
    pool::proto::Reply rep;
    if (type != zmtpMessage || !checkRequest(req, rep, connection->Stream.data(), connection->Stream.remaining())) {
      delete connection;
      return;
    }

    pool::proto::Request::Type requestType = req.type();
    ThreadData &data = Data_[GetLocalThreadId()];
    if (requestType == pool::proto::Request::SETCONFIG) {
      onSetConfig(data, connection, req, rep);
    } else if (requestType == pool::proto::Request::GETWORK) {
      onGetWork(data, connection, req, rep);
    } else if (requestType == pool::proto::Request::SHARE) {
      onShare(connection, data, req, rep);
    } else if (requestType == pool::proto::Request::STATS) {
      onStats(data, req, rep);
    }

    size_t repSize = rep.ByteSizeLong();
    connection->Stream.reset();
    // TODO: size limit check
    rep.SerializeToArray(connection->Stream.reserve(repSize), static_cast<int>(repSize));
    aioZmtpSend(connection->Socket, connection->Stream.data(), connection->Stream.sizeOf(), zmtpMessage, afNone, 0, nullptr, nullptr);
    aioZmtpRecv(connection->Socket, connection->Stream, 65536, afNone, 0, workerRecvCb, connection);
  }

private:
  std::unique_ptr<ThreadData[]> Data_;
  std::unordered_map<PoolBackend*, size_t> BackendMap_;
  unsigned CurrentWorker_ = 0;
  uint16_t ListenPort_ = 0;
  uint16_t WorkerPort_ = 0;
  std::string HostName_;
  typename X::Zmq::MiningConfig MiningCfg_;
  std::string Name_;
};
