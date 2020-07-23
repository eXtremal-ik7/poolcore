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
  if (!req.ParseFromArray(msg, msgSize)) {
    LOG_F(WARNING, "invalid message received");
    return false;
  }

  rep.Clear();
  rep.set_type(req.type());
  rep.set_reqid(req.reqid());
  rep.set_error(pool::proto::Reply::NONE);
  return true;
}

template<typename Proto>
class ZmqInstance : public CPoolInstance {
public:
  ZmqInstance(asyncBase *monitorBase, UserManager &userMgr, CThreadPool &threadPool, rapidjson::Value &config) : CPoolInstance(monitorBase, userMgr, threadPool) {
    Name_ = (std::string)Proto::TickerName + ".zmq";
    Data_.reset(new ThreadData[threadPool.threadsNum()]);
    for (unsigned i = 0; i < threadPool.threadsNum(); i++) {
      Data_[i].WorkerBase = threadPool.getBase(i);
      Data_[i].HasWork = false;
      Proto::checkConsensusInitialize(Data_[i].CheckConsensusCtx);
    }

    if (!(config.HasMember("port") && config["port"].IsInt() &&
          config.HasMember("workerPort") && config["workerPort"].IsInt() &&
          config.HasMember("hostName") && config["hostName"].IsString() &&
          config.HasMember("minShareLength") && config["minShareLength"].IsUint())) {
      LOG_F(ERROR, "instance %s: can't read 'port', 'workerPort', 'hostName', 'minShareLength' values from config", "XPM/zmq");
      exit(1);
    }

    uint16_t port = config["port"].GetInt();
    WorkerPort_ = config["workerPort"].GetInt();
    HostName_ = config["hostName"].GetString();
    MinShareLength_ = config["minShareLength"].GetUint();

    // Frontend listener
    createListener(monitorBase, port, [](socketTy socket, HostAddress, void *arg) { static_cast<ZmqInstance*>(arg)->newFrontendConnection(socket); }, this);

    // Worker/signal listeners (2*<worker num> ports used)
    for (unsigned i = 0; i < threadPool.threadsNum(); i++) {
      createListener(Data_[i].WorkerBase, WorkerPort_ + i*2, [](socketTy socket, HostAddress address, void *arg) { static_cast<ZmqInstance*>(arg)->newWorkerConnection(socket, address); }, this);
      createListener(Data_[i].WorkerBase, WorkerPort_ + i*2 + 1, [](socketTy socket, HostAddress, void *arg) { static_cast<ZmqInstance*>(arg)->newSignalsConnection(socket); }, this);
    }
  }

  virtual void checkNewBlockTemplate(rapidjson::Value &blockTemplate, PoolBackend *backend) override {
    intrusive_ptr<CSingleWorkInstance<Proto>> work = ::checkNewBlockTemplate<Proto>(blockTemplate, backend->getConfig(), backend->getCoinInfo(), SerializeBuffer_, Name_);
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
    AcceptWork(ZmqInstance &instance, intrusive_ptr<CSingleWorkInstance<Proto>> work, PoolBackend *backend) : Instance_(instance), Work_(work), Backend_(backend) {}
    void run(unsigned workerId) final { Instance_.acceptWork(workerId, Work_, Backend_); }
  private:
    ZmqInstance &Instance_;
    intrusive_ptr<CSingleWorkInstance<Proto>> Work_;
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
  };

  struct ThreadData {
    asyncBase *WorkerBase;
    typename Proto::CheckConsensusCtx CheckConsensusCtx;
    typename Proto::Block Block;
    PoolBackend *Backend;
    bool HasWork;
    uint64_t Height;
    size_t ExtraNonceOffset;
    std::set<Connection*> SignalSockets;
    std::unordered_map<uint256, CExtraNonce> ExtraNonceMap;
    std::unordered_set<uint256> KnownShares;
    std::unordered_set<uint256> KnownBlocks;
    xmstream Bin;
  };

private:
  void sendWork(ThreadData &data, Connection *connection) {
    // Increment extra nonce
    CExtraNonce extraNonce;
    incrementExtraNonce<Proto>(data.Block, data.ExtraNonceOffset, ThreadPool_.threadsNum(), extraNonce);
    data.ExtraNonceMap[data.Block.header.hashMerkleRoot] = extraNonce;

    // Fill block protobuf
    pool::proto::Signal sig;
    pool::proto::Block* block = sig.mutable_block();
    sig.set_type(pool::proto::Signal::NEWBLOCK);
    block->set_height(data.Height);
    block->set_hash(data.Block.header.hashPrevBlock.ToString());
    block->set_prevhash(data.Block.header.hashPrevBlock.ToString());
    block->set_reqdiff(0);
    block->set_minshare(MinShareLength_);

    // Fill work protobuf
    pool::proto::Work* work = sig.mutable_work();
    work->set_height(data.Height);
    work->set_merkle(data.Block.header.hashMerkleRoot.ToString().c_str());
    work->set_time(std::max(static_cast<uint32_t>(time(0)), data.Block.header.nTime));
    work->set_bits(data.Block.header.nBits);

    size_t repSize = sig.ByteSize();
    connection->Stream.reset();
    connection->Stream.template write<uint8_t>(1);
    sig.SerializeToArray(connection->Stream.reserve(repSize), repSize);
    aioZmtpSend(connection->Socket, connection->Stream.data(), connection->Stream.sizeOf(), zmtpMessage, afNone, 0, [](AsyncOpStatus status, zmtpSocket*, void *arg) {
      if (status != aosSuccess)
        delete static_cast<Connection*>(arg);
    }, connection);
  }

  void onGetWork(ThreadData &data, pool::proto::Request &req, pool::proto::Reply &rep) {
    if (data.HasWork) {
      // Increment extra nonce
      CExtraNonce extraNonce;
      incrementExtraNonce<Proto>(data.Block, data.ExtraNonceOffset, ThreadPool_.threadsNum(), extraNonce);
      data.ExtraNonceMap[data.Block.header.hashMerkleRoot] = extraNonce;

      // Set work
      pool::proto::Work* work = rep.mutable_work();
      work->set_height(data.Height);
      work->set_merkle(data.Block.header.hashMerkleRoot.ToString().c_str());
      work->set_time(std::max(static_cast<uint32_t>(time(0)), data.Block.header.nTime));
      work->set_bits(data.Block.header.nBits);
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

    // minimal share length
    if (share.length() < MinShareLength_) {
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }

    typename Proto::BlockHeader &header = data.Block.header;
    header.nTime = share.time();
    header.nNonce = share.nonce();
    header.hashMerkleRoot = uint256S(share.merkle());;
    header.bnPrimeChainMultiplier.set_str(share.multi().c_str(), 16);

    // merkle must be present at extraNonce map
    auto nonceIt = data.ExtraNonceMap.find(header.hashMerkleRoot);
    if (nonceIt == data.ExtraNonceMap.end()) {
      rep.set_error(pool::proto::Reply::STALE);
      return;
    }

    // candidate type must be valid
    unsigned nCandidateType = share.chaintype();
    if (nCandidateType > 2) {
      rep.set_error(pool::proto::Reply::INVALID);
      return;
    }

    // check duplicate
    typename Proto::BlockHashTy blockHash = header.GetHash();
    if (!data.KnownShares.insert(blockHash).second) {
      rep.set_error(pool::proto::Reply::DUPLICATE);
      return;
    }

    // check proof of work
    uint64_t shareSize;
    typename Proto::ChainParams chainParams;
    chainParams.minimalChainLength = 2;
    bool isBlock = Proto::checkConsensus(data.Block.header, data.CheckConsensusCtx, chainParams, &shareSize);

    // Send share to backend
    CAccountingShare *backendShare = new CAccountingShare;
    backendShare->userId = share.addr();
    backendShare->height = data.Height;
    backendShare->value = 1;
    data.Backend->sendShare(backendShare);

    if (isBlock) {
      LOG_F(INFO, "%s: new proof of work found hash: %s transactions: %zu", Name_.c_str(), blockHash.ToString().c_str(), data.Block.vtx.size());
      // Serialize block
      data.Bin.reset();
      BTC::serialize(data.Bin, data.Block);
      // Submit to nodes
      std::string addr = share.addr();
      uint64_t height = data.Height;
      int64_t generatedCoins = data.Block.vtx[0].txOut[0].value;
      CNetworkClientDispatcher &dispatcher = data.Backend->getClientDispatcher();
      dispatcher.aioSubmitBlock(data.WorkerBase, data.Bin.data(), data.Bin.sizeOf(), [addr, height, blockHash, generatedCoins, &data](bool result, const std::string error) {
        if (result) {
          if (!data.KnownBlocks.insert(blockHash).second)
            return;
          // Send block to backend
          CAccountingBlock *block = new CAccountingBlock;
          block->userId = addr;
          block->height = height;
          block->value = 1;
          block->hash = blockHash.ToString();
          block->generatedCoins = generatedCoins;
          data.Backend->sendBlock(block);
          LOG_F(INFO, " * block accepted by node");
        } else {
          LOG_F(ERROR, " * block rejected by node: %s", error.c_str());
        }
      });
    }
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
    aioZmtpAccept(connection->Socket, afNone, 5000000, [](AsyncOpStatus status, zmtpSocket *socket, void *arg) {
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

  void acceptWork(unsigned, intrusive_ptr<CSingleWorkInstance<Proto>> work, PoolBackend *backend) {
    ThreadData &data = Data_[GetLocalThreadId()];
    if (!work.get()) {
      data.HasWork = false;
      return;
    }

    // TODO: don't do deep copy
    data.Block = work.get()->Block;
    data.ExtraNonceOffset = work.get()->ExtraNonceOffset;
    data.Height = work.get()->Height;
    data.HasWork = true;
    data.Backend = backend;
    initializeExtraNonce<Proto>(data.Block, data.ExtraNonceOffset, GetLocalThreadId());
    data.ExtraNonceMap.clear();
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

    size_t repSize = rep.ByteSize();
    connection->Stream.reset();
    rep.SerializeToArray(connection->Stream.reserve(repSize), repSize);
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
      onGetWork(data, req, rep);
    } else if (requestType == pool::proto::Request::SHARE) {
      onShare(data, req, rep);
    } else if (requestType == pool::proto::Request::STATS) {

    }

    size_t repSize = rep.ByteSize();
    connection->Stream.reset();
    rep.SerializeToArray(connection->Stream.reserve(repSize), repSize);
    aioZmtpSend(connection->Socket, connection->Stream.data(), connection->Stream.sizeOf(), zmtpMessage, afNone, 0, nullptr, nullptr);
    aioZmtpRecv(connection->Socket, connection->Stream, 65536, afNone, 0, workerRecvCb, connection);
  }

private:
  std::unique_ptr<ThreadData[]> Data_;
  unsigned CurrentWorker_ = 0;
  uint16_t WorkerPort_ = 0;
  std::string HostName_;
  unsigned MinShareLength_;
  std::string Name_;
  xmstream SerializeBuffer_;
};
