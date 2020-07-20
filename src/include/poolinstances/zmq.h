#pragma once

#include "common.h"
#include "poolcore/backend.h"
#include "poolcore/poolCore.h"
#include "poolcore/poolInstance.h"
#include "poolcore/thread.h"
#include "blockmaker/merkleTree.h"
#include <asyncio/socket.h>
#include <asyncioextras/zmtp.h>
#include "protocol.pb.h"

using ListenerCallback = std::function<void(socketTy, HostAddress, void*)>;

struct ListenerContext {
   ListenerCallback Callback;
  void *Arg;
};

static void listenerAcceptCb(AsyncOpStatus status, aioObject *object, HostAddress address, socketTy socket, void *arg)
{
  if (status == aosSuccess) {
    ListenerContext *ctx = static_cast<ListenerContext*>(arg);
    ctx->Callback(socket, address, ctx->Arg);
  }

  aioAccept(object, 0, listenerAcceptCb, arg);
}

static void createListener(asyncBase *base, uint16_t port, ListenerCallback callback, void *arg)
{
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = htons(port);
  socketTy hSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  socketReuseAddr(hSocket);
  if (socketBind(hSocket, &address) != 0) {
    LOG_F(ERROR, "cannot bind port: %i", port);
    exit(1);
  }

  if (socketListen(hSocket) != 0) {
    LOG_F(ERROR, "listen error: %i", port);
    exit(1);
  }

  aioObject *object = newSocketIo(base, hSocket);

  ListenerContext *context = new ListenerContext;
  context->Callback = callback;
  context->Arg = arg;
  aioAccept(object, 0, listenerAcceptCb, context);
}

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
  ZmqInstance(asyncBase *base, CThreadPool &threadPool, rapidjson::Value &config) : CPoolInstance(base, threadPool) {
    Name_ = (std::string)Proto::TickerName + ".zmq";
    Data_.reset(new ThreadData[threadPool.threadsNum()]);
    for (unsigned i = 0; i < threadPool.threadsNum(); i++) {
      Data_[i].WorkerBase = threadPool.getBase(i);
      Data_[i].HasWork = false;
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

    // Frontend listener
    createListener(base, port, [](socketTy socket, HostAddress, void *arg) { static_cast<ZmqInstance*>(arg)->newFrontendConnection(socket); }, this);

    // Worker/signal listeners (2*<worker num> ports used)
    for (unsigned i = 0; i < threadPool.threadsNum(); i++) {
      createListener(Data_[i].WorkerBase, WorkerPort_ + i*2, [](socketTy socket, HostAddress address, void *arg) { static_cast<ZmqInstance*>(arg)->newWorkerConnection(socket, address); }, this);
      createListener(Data_[i].WorkerBase, WorkerPort_ + i*2 + 1, [](socketTy socket, HostAddress address, void *arg) { static_cast<ZmqInstance*>(arg)->newSignalsConnection(socket); }, this);
    }
  }

  virtual void checkNewBlockTemplate(rapidjson::Value &blockTemplate, PoolBackend *backend) override {
    intrusive_ptr<CSingleWorkInstance<Proto>> work = ::checkNewBlockTemplate<Proto>(blockTemplate, backend->getConfig(), backend->getCoinInfo(), SerializeBuffer_, Name_);
    if (!work.get())
      return;
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, work));
  }

  virtual void stopWork() override {
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, nullptr));
  }

private:
  class AcceptWork : public CThreadPool::Task {
  public:
    AcceptWork(ZmqInstance &instance, intrusive_ptr<CSingleWorkInstance<Proto>> work) : Instance_(instance), Work_(work) {}
    void run(unsigned workerId) final { Instance_.acceptWork(workerId, Work_); }
  private:
    ZmqInstance &Instance_;
    intrusive_ptr<CSingleWorkInstance<Proto>> Work_;
  };

  struct Connection {
    ZmqInstance *Instance;
    zmtpSocket *Socket;
    zmtpStream Stream;
    zmtpUserMsgTy MsgType;
    ~Connection() { zmtpSocketDelete(Socket); }
  };

  struct WorkerConnection {
    ZmqInstance *Instance;
    zmtpSocket *Socket;
    zmtpStream Stream;
    zmtpUserMsgTy MsgType;
    pool::proto::Request req;
    pool::proto::Reply rep;
  };

  struct ThreadData {
    asyncBase *WorkerBase;
    typename Proto::Block block;
    bool HasWork;
    uint64_t Height;
    size_t ExtraNonceOffset;
    std::set<Connection*> SignalSockets;
  };


private:
  void sendWork(ThreadData &data, Connection *connection) {
    // Increment extra nonce
    typename Proto::TxIn txIn = data.block.vtx[0].txIn[0];
    uint8_t *txInData = txIn.scriptSig.data();
    uint64_t *extraNonce = reinterpret_cast<uint64_t*>(txInData + data.ExtraNonceOffset);
    if (++extraNonce[0] == 0)
      ++extraNonce[1];
    // Calculate merkle root
    data.block.vtx[0].Hash.SetNull();
    data.block.header.hashMerkleRoot = calculateMerkleRoot<Proto>(data.block.vtx);

    pool::proto::Signal sig;
    pool::proto::Block* block = sig.mutable_block();
    sig.set_type(pool::proto::Signal::NEWBLOCK);
    block->set_height(data.Height);
    block->set_hash("0000000000000000000000000000000000000000000000000000000000000000");
    block->set_prevhash(data.block.header.hashPrevBlock.ToString());
    block->set_reqdiff(0);
    block->set_minshare(7);

    pool::proto::Work* work = sig.mutable_work();
    work->set_height(data.Height);
    work->set_merkle(data.block.header.hashMerkleRoot.ToString().c_str());
    work->set_time(std::max(static_cast<uint32_t>(time(0)), data.block.header.nTime));
    work->set_bits(data.block.header.nBits);

    size_t repSize = sig.ByteSize();
    connection->Stream.reset();
    connection->Stream.template write<uint8_t>(1);
    sig.SerializeToArray(connection->Stream.reserve(repSize), repSize);
    aioZmtpSend(connection->Socket, connection->Stream.data(), connection->Stream.sizeOf(), zmtpMessage, afNone, 0, nullptr, nullptr);
  }

  void newFrontendConnection(socketTy fd) {
    Connection *connection = new Connection;
    connection->Instance = this;
    connection->Socket = zmtpSocketNew(MonitorBase_, newSocketIo(MonitorBase_, fd), zmtpSocketDEALER);
    aioZmtpAccept(connection->Socket, afNone, 5000000, [](AsyncOpStatus status, zmtpSocket*, void *arg) {
      Connection *connection = static_cast<Connection*>(arg);
      if (status == aosSuccess)
        aioZmtpRecv(connection->Socket, connection->Stream, 65536, afNone, 0, frontendRecvCb, connection);
      else {
        delete connection;
      }
    }, connection);
  }

  void newWorkerConnection(socketTy fd, HostAddress address) {
    ThreadData &data = Data_[GetLocalThreadId()];
    WorkerConnection *connection = new WorkerConnection;
    connection->Instance = this;
    connection->Socket = zmtpSocketNew(data.WorkerBase, newSocketIo(data.WorkerBase, fd), zmtpSocketROUTER);
    aioZmtpAccept(connection->Socket, afNone, 5000000, [](AsyncOpStatus status, zmtpSocket *socket, void *arg) {
      WorkerConnection *connection = static_cast<WorkerConnection*>(arg);
      if (status == aosSuccess)
        aioZmtpRecv(connection->Socket, connection->Stream, 65536, afNone, 5000000, workerRecvCb, connection);
      else {
        delete connection;
      }
    }, connection);
  }

  void newSignalsConnection(socketTy fd) {
    ThreadData &data = Data_[GetLocalThreadId()];
    Connection *connection = new Connection;
    connection->Instance = this;
    connection->Socket = zmtpSocketNew(data.WorkerBase, newSocketIo(data.WorkerBase, fd), zmtpSocketPUB);
    aioZmtpAccept(connection->Socket, afNone, 5000000, [](AsyncOpStatus status, zmtpSocket *socket, void *arg) {
      Connection *connection = static_cast<Connection*>(arg);
      if (status == aosSuccess) {
        ThreadData &data = connection->Instance->Data_[GetLocalThreadId()];
        if (data.HasWork)
          connection->Instance->sendWork(data, connection);
        data.SignalSockets.insert(connection);
      } else {
        delete connection;
      }
    }, connection);
  }

  void acceptWork(unsigned, intrusive_ptr<CSingleWorkInstance<Proto>> work) {
    ThreadData &data = Data_[GetLocalThreadId()];
    if (!work.get()) {
      data.HasWork = false;
      return;
    }

    // TODO: don't do deep copy
    data.block = work.get()->Block;
    data.ExtraNonceOffset = work.get()->ExtraNonceOffset;
    data.Height = work.get()->Height;
    data.HasWork = true;

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
    WorkerConnection *connection = static_cast<WorkerConnection*>(arg);
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

  void workerProc(WorkerConnection *connection, zmtpUserMsgTy type) {
    if (type != zmtpMessage || !checkRequest(connection->req, connection->rep, connection->Stream.data(), connection->Stream.remaining())) {
      delete connection;
      return;
    }

    bool ready = true;
    pool::proto::Request::Type requestType = connection->req.type();
    ThreadData &data = Data_[GetLocalThreadId()];
    if (requestType == pool::proto::Request::GETWORK) {
      if (data.HasWork) {

      } else {
        std::string error = Name_ + ": no network connection";
        connection->rep.set_error(pool::proto::Reply::HEIGHT);
        connection->rep.set_errstr(error);
      }

      // Increment extra nonce
      typename Proto::TxIn txIn = data.block.vtx[0].txIn[0];
      uint8_t *txInData = txIn.scriptSig.data();
      uint64_t *extraNonce = reinterpret_cast<uint64_t*>(txInData + data.ExtraNonceOffset);
      if (++extraNonce[0] == 0)
        ++extraNonce[1];
      // Calculate merkle root
      data.block.vtx[0].Hash.SetNull();
      data.block.header.hashMerkleRoot = calculateMerkleRoot<Proto>(data.block.vtx);

      // Set work
      pool::proto::Work* work = connection->rep.mutable_work();
      work->set_height(data.Height);
      work->set_merkle(data.block.header.hashMerkleRoot.ToString().c_str());
      work->set_time(std::max(static_cast<uint32_t>(time(0)), data.block.header.nTime));
      work->set_bits(data.block.header.nBits);
    } else if (requestType == pool::proto::Request::SHARE) {

    } else if (requestType == pool::proto::Request::STATS) {

    }

    if (ready) {
      size_t repSize = connection->rep.ByteSize();
      connection->Stream.reset();
      connection->rep.SerializeToArray(connection->Stream.reserve(repSize), repSize);
      aioZmtpSend(connection->Socket, connection->Stream.data(), connection->Stream.sizeOf(), zmtpMessage, afNone, 0, nullptr, nullptr);
      aioZmtpRecv(connection->Socket, connection->Stream, 65536, afNone, 0, workerRecvCb, connection);
    }
  }

private:
  std::unique_ptr<ThreadData[]> Data_;
  unsigned CurrentWorker_ = 0;
  uint16_t WorkerPort_ = 0;
  std::string HostName_;
  std::string Name_;
  xmstream SerializeBuffer_;
};
