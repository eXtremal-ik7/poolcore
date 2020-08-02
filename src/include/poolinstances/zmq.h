#pragma once

#include "common.h"
#include "poolcore/backend.h"
#include "poolcore/poolCore.h"
#include "poolcore/poolInstance.h"
#include "poolcore/thread.h"
#include "poolcore/usermgr.h"
#include "blockmaker/merkleTree.h"
#include <asyncio/socket.h>
#include <asyncioextras/zmtp.h>
#include "protocol.pb.h"


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
  ZmqInstance(asyncBase *monitorBase, UserManager &userMgr, CThreadPool &threadPool, rapidjson::Value &config) : CPoolInstance(monitorBase, userMgr, threadPool) {
    Name_ = (std::string)X::Proto::TickerName + ".zmq";
    Data_.reset(new ThreadData[threadPool.threadsNum()]);
    for (unsigned i = 0; i < threadPool.threadsNum(); i++) {
      Data_[i].WorkerBase = threadPool.getBase(i);
      Data_[i].HasWork = false;
      X::Proto::checkConsensusInitialize(Data_[i].CheckConsensusCtx);
    }

    if (!(config.HasMember("port") && config["port"].IsInt() &&
          config.HasMember("workerPort") && config["workerPort"].IsInt() &&
          config.HasMember("hostName") && config["hostName"].IsString())) {
      LOG_F(ERROR, "instance %s: can't read 'port', 'workerPort', 'hostName' values from config", "XPM/zmq");
      exit(1);
    }

    uint16_t port = config["port"].GetInt();
    WorkerPort_ = config["workerPort"].GetInt();
    HostName_ = config["hostName"].GetString();

    X::Zmq::initializeMiningConfig(MiningCfg_, config);

    // Frontend listener
    createListener(monitorBase, port, [](socketTy socket, HostAddress, void *arg) { static_cast<ZmqInstance*>(arg)->newFrontendConnection(socket); }, this);

    // Worker/signal listeners (2*<worker num> ports used)
    for (unsigned i = 0; i < threadPool.threadsNum(); i++) {
      createListener(Data_[i].WorkerBase, WorkerPort_ + i*2, [](socketTy socket, HostAddress address, void *arg) { static_cast<ZmqInstance*>(arg)->newWorkerConnection(socket, address); }, this);
      createListener(Data_[i].WorkerBase, WorkerPort_ + i*2 + 1, [](socketTy socket, HostAddress, void *arg) { static_cast<ZmqInstance*>(arg)->newSignalsConnection(socket); }, this);
    }
  }

  virtual void checkNewBlockTemplate(rapidjson::Value &blockTemplate, PoolBackend *backend) override {
    std::string error;
    typename X::Zmq::MiningConfig config = MiningCfg_;
    if (!X::Zmq::buildFullMiningConfig(config, backend->getConfig(), backend->getCoinInfo()))
      return;

    auto work = X::Zmq::loadFromTemplate(blockTemplate, config, error);
    if (!work.get())
      return;
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, work, backend));
  }

  virtual void stopWork() override {
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, nullptr, nullptr));
  }

private:
  class AcceptWork : public CThreadPool::Task {
  public:
    AcceptWork(ZmqInstance &instance, intrusive_ptr<typename X::Zmq::Work> work, PoolBackend *backend) : Instance_(instance), Work_(work), Backend_(backend) {}
    void run(unsigned workerId) final { Instance_.acceptWork(workerId, Work_, Backend_); }
  private:
    ZmqInstance &Instance_;
    intrusive_ptr<typename X::Zmq::Work> Work_;
    PoolBackend *Backend_;
  };

  struct Connection {
    Connection(ZmqInstance *instance, zmtpSocket *socket, unsigned workerId, bool isSignal) : Instance(instance), Socket(socket), WorkerId(workerId), IsSignal(isSignal), IsConnected(false) {
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
    typename X::Zmq::Work Work;
    typename X::Zmq::ThreadConfig ThreadConfig;
    PoolBackend *Backend;
    bool HasWork;
    uint64_t Height;
    std::set<Connection*> SignalSockets;
    std::unordered_set<uint256> KnownShares;
    std::unordered_set<uint256> KnownBlocks;
    xmstream Bin;
  };

private:
  void sendWork(ThreadData &data, Connection *connection) {
    X::Zmq::generateNewWork(data.Work, connection->WorkerConfig, data.ThreadConfig, MiningCfg_);

    // Fill block protobuf
    pool::proto::Signal sig;
    sig.set_type(pool::proto::Signal::NEWBLOCK);
    X::Zmq::buildBlockProto(data.Work, connection->WorkerConfig, data.ThreadConfig, MiningCfg_, *sig.mutable_block());
    X::Zmq::buildWorkProto(data.Work, connection->WorkerConfig, data.ThreadConfig, MiningCfg_, *sig.mutable_work());

    size_t repSize = sig.ByteSizeLong();
    connection->Stream.reset();
    connection->Stream.template write<uint8_t>(1);
    // TODO: size limit check
    sig.SerializeToArray(connection->Stream.reserve(repSize), static_cast<int>(repSize));
    aioZmtpSend(connection->Socket, connection->Stream.data(), connection->Stream.sizeOf(), zmtpMessage, afNone, 0, [](AsyncOpStatus status, zmtpSocket*, void *arg) {
      if (status != aosSuccess)
        delete static_cast<Connection*>(arg);
    }, connection);
  }

  void onGetWork(ThreadData &data, Connection *connection, pool::proto::Request&, pool::proto::Reply &rep) {
    if (data.HasWork) {
      X::Zmq::generateNewWork(data.Work, connection->WorkerConfig, data.ThreadConfig, MiningCfg_);
      X::Zmq::buildWorkProto(data.Work, connection->WorkerConfig, data.ThreadConfig, MiningCfg_, *rep.mutable_work());
    } else {
      std::string error = Name_ + ": no network connection";
      rep.set_error(pool::proto::Reply::HEIGHT);
      rep.set_errstr(error);
    }
  }

  void onShare(ThreadData &data, pool::proto::Request &req, pool::proto::Reply &rep) {
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

    // check user name
    // TODO: move it to connection
    UserManager::Credentials credentials;
    if (!UserMgr_.getUserCredentials(share.addr(), credentials)) {
      rep.set_errstr((std::string)"Unknown user " + share.addr());
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }

    // block height
    if (share.height() != data.Height) {
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

    int64_t blockReward = 0;
    if (!X::Zmq::prepareToSubmit(data.Work, data.ThreadConfig, MiningCfg_, req, rep, &blockReward))
      return;

    // check duplicate
    typename X::Proto::BlockHashTy blockHash = data.Work.Block.header.GetHash();
    if (!data.KnownShares.insert(blockHash).second) {
      rep.set_error(pool::proto::Reply::DUPLICATED);
      return;
    }

    // check proof of work
    uint64_t shareSize;
    typename X::Proto::ChainParams chainParams;
    chainParams.minimalChainLength = 2;
    bool isBlock = X::Proto::checkConsensus(data.Work.Block.header, data.CheckConsensusCtx, chainParams, &shareSize);

    if (isBlock) {
      LOG_F(INFO, "%s: new proof of work found hash: %s transactions: %zu", Name_.c_str(), blockHash.ToString().c_str(), data.Work.Block.vtx.size());
      // Serialize block
      data.Bin.reset();
      BTC::serialize(data.Bin, data.Work.Block);
      // Submit to nodes
      uint64_t height = data.Height;
      std::string user = share.addr();
      CNetworkClientDispatcher &dispatcher = data.Backend->getClientDispatcher();
      dispatcher.aioSubmitBlock(data.WorkerBase, data.Bin.data(), data.Bin.sizeOf(), [height, user, blockHash, blockReward, &data](bool result, const std::string &hostName, const std::string &error) {
        if (result) {
          LOG_F(INFO, "* block %s (%" PRIu64 ") accepted by %s", blockHash.ToString().c_str(), height, hostName.c_str());
          if (data.KnownBlocks.insert(blockHash).second) {
            // Send share with block to backend
            CAccountingShare *backendShare = new CAccountingShare;
            backendShare->userId = user;
            backendShare->height = data.Height;
            backendShare->value = 1;
            backendShare->isBlock = true;
            backendShare->hash = blockHash.ToString();
            backendShare->generatedCoins = blockReward;
            data.Backend->sendShare(backendShare);
          }
        } else {
          LOG_F(ERROR, "* block %s (%" PRIu64 ") rejected by %s error: %s", blockHash.ToString().c_str(), height, hostName.c_str(), error.c_str());
        }
      });
    } else {
      // Send share to backend
      CAccountingShare *backendShare = new CAccountingShare;
      backendShare->userId = share.addr();
      backendShare->height = data.Height;
      backendShare->value = 1;
      backendShare->isBlock = false;
      data.Backend->sendShare(backendShare);
    }
  }

  void onStats(ThreadData &data, pool::proto::Request &req, pool::proto::Reply &rep) {
    if (!req.has_stats()) {
      LOG_F(WARNING, "!req.has_stats()");
      return;
    }

    const pool::proto::ClientStats &src = req.stats();

    // check user name
    // TODO: move it to connection
    UserManager::Credentials credentials;
    if (!UserMgr_.getUserCredentials(src.addr(), credentials)) {
      rep.set_errstr((std::string)"Unknown user " + src.addr());
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }

    CUserStats *stats = new CUserStats;
    stats->userId = src.addr();
    stats->workerId = src.name();
    stats->power = (int64_t)(src.cpd()*1000.0);
    stats->latency = src.latency();
    // TODO: fill with IP
    stats->address = "<unknown>";
    stats->type = EUnitTypeGPU;
    stats->units = src.ngpus();
    stats->temp = src.temp();
    data.Backend->sendStats(stats);
  }

  void newFrontendConnection(socketTy fd) {
    Connection *connection = new Connection(this, zmtpSocketNew(MonitorBase_, newSocketIo(MonitorBase_, fd), zmtpSocketDEALER), -1, false);
    aioZmtpAccept(connection->Socket, afNone, 5000000, [](AsyncOpStatus status, zmtpSocket*, void *arg) {
      Connection *connection = static_cast<Connection*>(arg);
      if (status == aosSuccess) {
        connection->IsConnected = true;
        aioZmtpRecv(connection->Socket, connection->Stream, 65536, afNone, 0, frontendRecvCb, connection);
       } else {
        delete connection;
      }
    }, connection);
  }

  void newWorkerConnection(socketTy fd, HostAddress) {
    ThreadData &data = Data_[GetLocalThreadId()];
    Connection *connection = new Connection(this, zmtpSocketNew(data.WorkerBase, newSocketIo(data.WorkerBase, fd), zmtpSocketROUTER), GetLocalThreadId(), false);
    aioZmtpAccept(connection->Socket, afNone, 5000000, [](AsyncOpStatus status, zmtpSocket*, void *arg) {
      Connection *connection = static_cast<Connection*>(arg);
      if (status == aosSuccess) {
        connection->IsConnected = true;
        aioZmtpRecv(connection->Socket, connection->Stream, 65536, afNone, 5000000, workerRecvCb, connection);
      } else {
        delete connection;
      }
    }, connection);
  }

  void newSignalsConnection(socketTy fd) {
    ThreadData &data = Data_[GetLocalThreadId()];
    Connection *connection = new Connection(this, zmtpSocketNew(data.WorkerBase, newSocketIo(data.WorkerBase, fd), zmtpSocketPUB), GetLocalThreadId(), true);
    aioZmtpAccept(connection->Socket, afNone, 5000000, [](AsyncOpStatus status, zmtpSocket*, void *arg) {
      Connection *connection = static_cast<Connection*>(arg);
      if (status == aosSuccess) {
        connection->IsConnected = true;
        ThreadData &data = connection->Instance->Data_[GetLocalThreadId()];
        if (data.HasWork)
          connection->Instance->sendWork(data, connection);
      } else {
        delete connection;
      }
    }, connection);
  }

  void acceptWork(unsigned, intrusive_ptr<typename X::Zmq::Work> work, PoolBackend *backend) {
    ThreadData &data = Data_[GetLocalThreadId()];
    if (!work.get()) {
      data.HasWork = false;
      return;
    }

    data.Work = *work.get();
    data.Height = work.get()->Height;
    data.HasWork = true;
    data.Backend = backend;

    X::Zmq::resetThreadConfig(data.ThreadConfig);
    data.KnownShares.clear();
    data.KnownBlocks.clear();

    // Send signals
    for (const auto &connection: data.SignalSockets) {
      sendWork(data, connection);
    }
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
      // TODO: check version
      bool versionIsValid = true;

      if (!versionIsValid) {
        rep.set_error(pool::proto::Reply::VERSION);
        rep.set_errstr("Your miner version will no longer be supported in the near future. Please upgrade.");
      }

      // fill 'bitcoin' and 'signals' host addresses
      pool::proto::ServerInfo mServerInfo;
      mServerInfo.set_host(HostName_);
      mServerInfo.set_router(WorkerPort_ + CurrentWorker_*2);
      mServerInfo.set_pub(WorkerPort_ + CurrentWorker_*2 + 1);
      // XPM specific
      // TODO: get from block template (nBits)
      mServerInfo.set_target(10);
      mServerInfo.set_versionmajor(10);
      mServerInfo.set_versionminor(3);
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
    if (requestType == pool::proto::Request::GETWORK) {
      onGetWork(data, connection, req, rep);
    } else if (requestType == pool::proto::Request::SHARE) {
      onShare(data, req, rep);
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
  unsigned CurrentWorker_ = 0;
  uint16_t WorkerPort_ = 0;
  std::string HostName_;
  typename X::Zmq::MiningConfig MiningCfg_;
  std::string Name_;
  xmstream SerializeBuffer_;
};
