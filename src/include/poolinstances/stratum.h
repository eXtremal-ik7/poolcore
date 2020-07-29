#pragma once

#include "common.h"
#include "stratumMsg.h"
#include "poolcommon/arith_uint256.h"
#include "poolcommon/jsonSerializer.h"
#include "poolcore/backend.h"
#include "poolcore/poolCore.h"
#include "poolcore/poolInstance.h"
#include <openssl/rand.h>
#include <rapidjson/writer.h>
#include <unordered_map>

static inline double getDifficulty(uint32_t bits)
{
    int nShift = (bits >> 24) & 0xff;
    double dDiff =
        (double)0x0000ffff / (double)(bits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

static inline void addId(JSON::Object &object, StratumMessage &msg) {
  if (!msg.stringId.empty())
    object.addString("id", msg.stringId);
  else
    object.addInt("id", msg.integerId);
}

template<typename Proto>
class StratumInstance : public CPoolInstance {
public:
  StratumInstance(asyncBase *monitorBase, UserManager &userMgr, CThreadPool &threadPool, rapidjson::Value &config) : CPoolInstance(monitorBase, userMgr, threadPool), CurrentThreadId_(0) {
    Name_ = (std::string)Proto::TickerName + ".stratum";
    Data_.reset(new ThreadData[threadPool.threadsNum()]);
    for (unsigned i = 0; i < threadPool.threadsNum(); i++) {
      Data_[i].WorkerBase = threadPool.getBase(i);
      Data_[i].ExtraNonceCurrent = i+12345;
      Data_[i].Height = 0;
      Proto::checkConsensusInitialize(Data_[i].CheckConsensusCtx);
    }

    if (!(config.HasMember("port") && config["port"].IsUint() &&
          config.HasMember("sharesPerBlock") && config["sharesPerBlock"].IsUint64())) {
      LOG_F(ERROR, "instance %s: can't read 'port', 'sharesPerBlock' values from config", Name_.c_str());
      exit(1);
    }

    uint16_t port = config["port"].GetInt();
    SharesPerBlock_ = config["sharesPerBlock"].GetUint64();

    if (config.HasMember("fixedExtraNonceSize") && config["fixedExtraNonceSize"].IsUint())
      FixedExtraNonceSize_ = config["fixedExtraNonceSize"].GetUint();
    if (config.HasMember("mutableExtraNonceSize") && config["mutableExtraNonceSize"].IsUint())
      MutableExtraNonceSize_ = config["mutableExtraNonceSize"].GetUint();
    if (config.HasMember("minShareDiff")) {
      if (config["minShareDiff"].IsUint64()) {
        MinShareDiff_ = config["minShareDiff"].GetUint64();
      } else if (config["minShareDiff"].IsFloat()) {
        MinShareDiff_ = config["minShareDiff"].GetFloat();
      } else {
        LOG_F(ERROR, "instance %s: invalid field 'minShareDiff' format");
        exit(1);
      }
    }

    // Main listener
    createListener(monitorBase, port, [](socketTy socket, HostAddress address, void *arg) { static_cast<StratumInstance*>(arg)->newFrontendConnection(socket, address); }, this);
  }

  virtual void checkNewBlockTemplate(rapidjson::Value &blockTemplate, PoolBackend *backend) override {
    intrusive_ptr<CSingleWorkInstance<Proto>> work = ::acceptBlockTemplate<Proto>(blockTemplate, backend->getConfig(), backend->getCoinInfo(), SerializeBuffer_, Name_, FixedExtraNonceSize_+MutableExtraNonceSize_);
    if (!work.get())
      return;
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, work, backend));
  }

  virtual void stopWork() override {
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, nullptr, nullptr));
  }

  void acceptWork(unsigned, intrusive_ptr<CSingleWorkInstance<Proto>> work, PoolBackend *backend) {
    ThreadData &data = Data_[GetLocalThreadId()];
    if (!work.get()) {
      data.JobSet.clear();
      return;
    }

    bool difficultyChanged = false;
    bool isNewBlock = data.Height != work.get()->Height || data.Backend != backend;
    if (isNewBlock) {
      // New block
      data.Height = work.get()->Height;
      data.Backend = backend;
      data.JobSet.clear();
      data.JobSet.emplace_back(work.get()->Block);

      // Set share difficulty
      double oldDifficulty = data.ShareDifficulty;
      data.ShareDifficulty = getDifficulty(work.get()->Block.header.nBits) / SharesPerBlock_;
      data.ShareDifficulty = std::max(data.ShareDifficulty, MinShareDiff_);
      difficultyChanged = oldDifficulty != data.ShareDifficulty;
      LOG_F(WARNING, "%s share difficulty %.2lf", Name_.c_str(), data.ShareDifficulty);
    } else {
      // Another work for same block
      data.JobSet.emplace_back(work.get()->Block);
    }

    data.KnownShares.clear();
    data.KnownBlocks.clear();

    Job &job = data.JobSet.back();
    job.ScriptSigExtraNonceOffset = work.get()->ScriptSigExtraNonceOffset;
    job.TxExtraNonceOffset = job.ScriptSigExtraNonceOffset + work.get()->Block.vtx[0].getFirstScriptSigOffset();
    buildNotifyMessage(job, data.Height, data.JobSet.size()-1, isNewBlock);

    if (difficultyChanged) {
      for (auto &connection: data.Connections_)
        stratumSendTarget(connection);
    }

    for (auto &connection: data.Connections_) {
      stratumSendWork(connection);
    }
  }

private:
  class AcceptWork : public CThreadPool::Task {
  public:
    AcceptWork(StratumInstance &instance, intrusive_ptr<CSingleWorkInstance<Proto>> work, PoolBackend *backend) : Instance_(instance), Work_(work), Backend_(backend) {}
    void run(unsigned workerId) final { Instance_.acceptWork(workerId, Work_, Backend_); }
  private:
    StratumInstance &Instance_;
    intrusive_ptr<CSingleWorkInstance<Proto>> Work_;
    PoolBackend *Backend_;
  };

  struct Worker {
    std::string User;
    std::string WorkerName;
  };

  struct Connection {
    Connection(StratumInstance *instance, aioObject *socket, unsigned workerId, HostAddress address) : Instance(instance), Socket(socket), WorkerId(workerId), Address(address) {
      Instance->Data_[WorkerId].Connections_.insert(this);
    }

    ~Connection() {
      Instance->Data_[WorkerId].Connections_.erase(this);
      deleteAioObject(Socket);
    }

    StratumInstance *Instance;
    // Network
    aioObject *Socket;
    unsigned WorkerId;
    HostAddress Address;
    // Stratum protocol decoding
    char Buffer[40960];
    unsigned Offset = 0;
    // Mining info
    uint64_t ExtraNonceFixed = 0;
    std::string SetDifficultySession;
    std::string NotifySession;
    std::unordered_map<std::string, Worker> Workers;
  };

  struct Job {
    typename Proto::Block Block;
    size_t ScriptSigExtraNonceOffset;
    size_t TxExtraNonceOffset;
    xmstream NotifyMessage;
    Job(const typename Proto::Block &block) : Block(block) {}
  };

  struct ThreadData {
    asyncBase *WorkerBase;
    typename Proto::CheckConsensusCtx CheckConsensusCtx;
    uint64_t ExtraNonceCurrent;
    uint64_t Height;
    PoolBackend *Backend;
    double ShareDifficulty;
    std::unordered_set<typename Proto::BlockHashTy> KnownShares;
    std::unordered_set<typename Proto::BlockHashTy> KnownBlocks;
    std::vector<Job> JobSet;
    std::set<Connection*> Connections_;
    xmstream Bin;
  };

private:
  void buildNotifyMessage(Job &job, uint64_t height, unsigned jobId, bool resetPreviousWork) {
    {
      job.NotifyMessage.reset();
      JSON::Object root(job.NotifyMessage);
      root.addNull("id");
      root.addString("method", "mining.notify");
      root.addField("params");
      {
        JSON::Array params(job.NotifyMessage);
        {
          // Id
          char buffer[32];
          snprintf(buffer, sizeof(buffer), "%" PRIu64 "#%u", height, jobId);
          params.addString(buffer);
        }

        // Previous block
        {
          std::string hash;
          const uint32_t *data = reinterpret_cast<const uint32_t*>(job.Block.header.hashPrevBlock.begin());
          for (unsigned i = 0; i < 8; i++) {
            hash.append(writeHexBE(data[i], 4));
          }
          params.addString(hash);
        }

        {
          // Coinbase Tx parts
          uint8_t buffer[1024];
          xmstream stream(buffer, sizeof(buffer));
          stream.reset();
          BTC::serialize(stream, job.Block.vtx[0]);

          // Part 1
          params.addHex(stream.data(), job.TxExtraNonceOffset);

          // Part 2
          size_t part2Offset = job.TxExtraNonceOffset + FixedExtraNonceSize_ + MutableExtraNonceSize_;
          size_t part2Size = stream.sizeOf() - job.TxExtraNonceOffset - (FixedExtraNonceSize_ + MutableExtraNonceSize_);
          params.addHex(stream.data<uint8_t>() + part2Offset, part2Size);
        }

        {
          // Merkle branches
          std::vector<typename Proto::BlockHashTy> merkleBranches;
          dumpMerkleTree<Proto>(job.Block.vtx, merkleBranches);
          params.addField();
          {
            JSON::Array branches(job.NotifyMessage);
            for (const auto &hash: merkleBranches)
              branches.addString(hash.ToString());
          }
        }
        // nVersion
        params.addString(writeHexBE(job.Block.header.nVersion, sizeof(job.Block.header.nVersion)));
        // nBits
        params.addString(writeHexBE(job.Block.header.nBits, sizeof(job.Block.header.nBits)));
        // nTime
        params.addString(writeHexBE(job.Block.header.nTime, sizeof(job.Block.header.nTime)));
        // cleanup
        params.addBoolean(resetPreviousWork);
      }
    }

    job.NotifyMessage.write('\n');
    {
      // TODO: WARNING -> DEBUG
      std::string msg(job.NotifyMessage.template data<char>(), job.NotifyMessage.sizeOf());
      LOG_F(WARNING, "%s", msg.c_str());
    }
  }

  void onStratumSubscribe(Connection *connection, StratumMessage &msg) {
    // Response format
    // {"id": 1, "result": [ [ ["mining.set_difficulty", <setDifficultySession>:string(hex)], ["mining.notify", <notifySession>:string(hex)]], <uniqueExtraNonce>:string(hex), extraNonceSize:integer], "error": null}\n

    char buffer[4096];
    xmstream stream(buffer, sizeof(buffer));
    stream.reset();
    {
      JSON::Object object(stream);
      addId(object, msg);
      object.addField("result");
      {
        JSON::Array result(stream);
        result.addField();
        {
          JSON::Array sessions(stream);
          sessions.addField();
          {
            JSON::Array setDifficultySession(stream);
            setDifficultySession.addString("mining.set_difficulty");
            setDifficultySession.addString(connection->SetDifficultySession);
          }
          sessions.addField();
          {
            JSON::Array notifySession(stream);
            notifySession.addString("mining.notify");
            notifySession.addString(connection->NotifySession);
          }
        }

        // Unique extra nonce
        result.addString(writeHexBE(connection->ExtraNonceFixed, FixedExtraNonceSize_));
        // Mutable part of extra nonce size
        result.addInt(MutableExtraNonceSize_);
      }
      object.addNull("error");
    }

    stream.write('\n');
    aioWrite(connection->Socket, stream.data(), stream.sizeOf(), afWaitAll, 0, nullptr, nullptr);
    {
      // TODO: WARNING -> DEBUG
      stream.write('\0');
      LOG_F(WARNING, "%s", stream.data<char>());
    }
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
      authSuccess = UserMgr_.checkPassword(worker.User, msg.authorize.password);
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
    aioWrite(connection->Socket, stream.data(), stream.sizeOf(), afWaitAll, 0, nullptr, nullptr);
    {
      // TODO: WARNING -> DEBUG
      stream.write('\0');
      LOG_F(WARNING, "%s", stream.data<char>());
    }
    return authSuccess;
  }

  void onStratumExtraNonceSubscribe(Connection *connection, StratumMessage &message) {
    xmstream stream;
    {
      JSON::Object object(stream);
      addId(object, message);
      object.addBoolean("result", false);
      object.addNull("error");
    }

    stream.write('\n');
    aioWrite(connection->Socket, stream.data(), stream.sizeOf(), afWaitAll, 0, nullptr, nullptr);
    {
      // TODO: WARNING -> DEBUG
      stream.write('\0');
      LOG_F(WARNING, "%s", stream.data<char>());
    }
  }

  void onStratumSubmit(Connection *connection, StratumMessage &msg) {
    ThreadData &data = Data_[GetLocalThreadId()];

    // Check worker name
    bool result = false;
    Worker *worker = nullptr;
    {
      auto It = connection->Workers.find(msg.submit.WorkerName);
      if (It != connection->Workers.end()) {
        worker = &It->second;
        result = true;
      }
    }

    // Check job id
    size_t jobIndex;
    if (result == true) {
      result = false;
      size_t sharpPos = msg.submit.JobId.find('#');
      if (sharpPos != msg.submit.JobId.npos) {
        msg.submit.JobId[sharpPos] = 0;
        uint64_t height = xatoi<uint64_t>(msg.submit.JobId.c_str());
        jobIndex = xatoi<size_t>(msg.submit.JobId.c_str() + sharpPos + 1);
        if (height == data.Height && jobIndex < data.JobSet.size())
          result = true;
      }
    }

    // Build header and check proof of work
    Job &job = data.JobSet[jobIndex];
    if (result == true && msg.submit.MutableExtraNonce.size() == MutableExtraNonceSize_) {
      result = false;
      // Write extra nonce into block
      typename Proto::Block &block = job.Block;
      typename Proto::BlockHeader &header = block.header;
      typename Proto::TxIn &txIn = block.vtx[0].txIn[0];

      uint8_t *scriptSig = txIn.scriptSig.data() + job.ScriptSigExtraNonceOffset;
      writeBinBE(connection->ExtraNonceFixed, FixedExtraNonceSize_, scriptSig);
      memcpy(scriptSig + FixedExtraNonceSize_, msg.submit.MutableExtraNonce.data(), msg.submit.MutableExtraNonce.size());

      // Calculate merkle root and build header
      block.vtx[0].Hash.SetNull();
      typename Proto::ChainParams chainParams;
      header.hashMerkleRoot = calculateMerkleRoot<Proto>(block.vtx);
      header.nTime = msg.submit.Time;
      header.nNonce = msg.submit.Nonce;
      typename Proto::BlockHashTy blockHash = header.GetHash();

      bool isBlock = Proto::checkConsensus(header, data.CheckConsensusCtx, chainParams);
      if (isBlock) {
        LOG_F(INFO, "%s: new proof of work found hash: %s transactions: %zu", Name_.c_str(), blockHash.ToString().c_str(), block.vtx.size());
        // Serialize block
        data.Bin.reset();
        BTC::serialize(data.Bin, block);
        uint64_t height = data.Height;
        std::string user = worker->User;
        int64_t generatedCoins = block.vtx[0].txOut[0].value;
        CNetworkClientDispatcher &dispatcher = data.Backend->getClientDispatcher();
        dispatcher.aioSubmitBlock(data.WorkerBase, data.Bin.data(), data.Bin.sizeOf(), [height, user, blockHash, generatedCoins, &data](bool result, const std::string &hostName, const std::string &error) {
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
              backendShare->generatedCoins = generatedCoins;
              data.Backend->sendShare(backendShare);
            }
          } else {
            LOG_F(ERROR, "* block %s (%" PRIu64 ") rejected by %s error: %s", blockHash.ToString().c_str(), height, hostName.c_str(), error.c_str());
          }
        });
      } else {
        // Send share to backend
        CAccountingShare *backendShare = new CAccountingShare;
        backendShare->userId = worker->User;
        backendShare->height = data.Height;
        backendShare->value = 1;
        backendShare->isBlock = false;
        data.Backend->sendShare(backendShare);
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
    aioWrite(connection->Socket, stream.data(), stream.sizeOf(), afWaitAll, 0, nullptr, nullptr);
    {
      // TODO: WARNING -> DEBUG
      stream.write('\0');
      LOG_F(WARNING, "%s", stream.data<char>());
    }
  }

  void stratumSendTarget(Connection *connection) {
    ThreadData &data = Data_[GetLocalThreadId()];
    xmstream stream;
    {
      JSON::Object object(stream);
      object.addNull("id");
      object.addString("method", "mining.set_difficulty");
      object.addField("params");
      {
        JSON::Array params(stream);
        params.addDouble(data.ShareDifficulty);
      }
    }

    stream.write('\n');
    aioWrite(connection->Socket, stream.data(), stream.sizeOf(), afWaitAll, 0, nullptr, nullptr);
    {
      // TODO: WARNING -> DEBUG
      stream.write('\0');
      LOG_F(WARNING, "%s", stream.data<char>());
    }
  }

  void stratumSendWork(Connection *connection) {
    ThreadData &data = Data_[GetLocalThreadId()];
    Job &job = data.JobSet.back();
    aioWrite(connection->Socket, job.NotifyMessage.data(), job.NotifyMessage.sizeOf(), afWaitAll, 0, nullptr, nullptr);
  }

  void newFrontendConnection(socketTy fd, HostAddress address) {
    ThreadData &data = Data_[CurrentThreadId_];
    CurrentThreadId_ = (CurrentThreadId_ + 1) % ThreadPool_.threadsNum();

    Connection *connection = new Connection(this, newSocketIo(data.WorkerBase, fd), CurrentThreadId_, address);

    // Set fixed part of extra nonce
    connection->ExtraNonceFixed = data.ExtraNonceCurrent;
    data.ExtraNonceCurrent += ThreadPool_.threadsNum();

    // Set session names
    uint8_t sessionId[16];
    char sessionIdHex[32];
    {
      RAND_bytes(sessionId, sizeof(sessionId));
      connection->SetDifficultySession.resize(sizeof(sessionId)*2);
      bin2hexLowerCase(sessionId, connection->SetDifficultySession.data(), sizeof(sessionId));
    }
    {
      RAND_bytes(sessionId, sizeof(sessionId));
      connection->NotifySession.resize(sizeof(sessionId)*2);
      bin2hexLowerCase(sessionId, connection->NotifySession.data(), sizeof(sessionId));
    }

    aioRead(connection->Socket, connection->Buffer, sizeof(connection->Buffer), afNone, 3000000, reinterpret_cast<aioCb*>(readCb), connection);
    CurrentThreadId_ = (CurrentThreadId_ + 1) % ThreadPool_.threadsNum();
  }

  static void readCb(AsyncOpStatus status, aioObject*, size_t size, Connection *connection) {
    if (status != aosSuccess) {
      delete connection;
      return;
    }

    connection->Offset += size;

    char *nextMsgPos = static_cast<char*>(memchr(connection->Buffer, '\n', connection->Offset));
    if (nextMsgPos) {
      // parse stratum message
      StratumMessage msg;
      bool result = true;
      size_t stratumMsgSize = nextMsgPos - connection->Buffer;
      switch (decodeStratumMessage(connection->Buffer, stratumMsgSize, &msg)) {
        case StratumDecodeStatusTy::Ok :
          // Process stratum messages here
          switch (msg.method) {
            case StratumMethodTy::Subscribe :
              connection->Instance->onStratumSubscribe(connection, msg);
              break;
            case StratumMethodTy::Authorize : {
              ThreadData &data = connection->Instance->Data_[connection->WorkerId];
              result = connection->Instance->onStratumAuthorize(connection, msg);
              if (result && !data.JobSet.empty()) {
                connection->Instance->stratumSendTarget(connection);
                connection->Instance->stratumSendWork(connection);
              }
              break;
            }
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
              LOG_F(ERROR, "%s: unknown stratum method received from %s" , connection->Instance->Name_.c_str(), inet_ntoa(addr));
              LOG_F(ERROR, " * message text: %s", connection->Buffer);
              break;
            }
          }

          break;
        case StratumDecodeStatusTy::JsonError :
          result = false;
          return;
        case StratumDecodeStatusTy::FormatError :
          result = false;
          return;
        default :
          break;
      }

      if (!result) {
        delete connection;
        return;
      }

      // move tail to begin of buffer
      ssize_t nextMsgOffset = nextMsgPos + 1 - connection->Buffer;
      if (nextMsgOffset < connection->Offset) {
        memcpy(connection->Buffer, connection->Buffer+nextMsgOffset, connection->Offset-nextMsgOffset);
        connection->Offset = connection->Offset - nextMsgOffset;
      } else {
        connection->Offset = 0;
      }
    }

    if (connection->Offset == sizeof(connection->Buffer)) {
      struct in_addr addr;
      addr.s_addr = connection->Address.ipv4;
      LOG_F(ERROR, "%s: too long stratum message from %s", connection->Instance->Name_.c_str(), inet_ntoa(addr));
      delete connection;
      return;
    }

    aioRead(connection->Socket, connection->Buffer + connection->Offset, sizeof(connection->Buffer) - connection->Offset, afNone, 0, reinterpret_cast<aioCb*>(readCb), connection);
  }

private:
  std::unique_ptr<ThreadData[]> Data_;

  std::string Name_;
  xmstream SerializeBuffer_;
  unsigned CurrentThreadId_;

  uint64_t SharesPerBlock_;
  unsigned FixedExtraNonceSize_ = 4;
  unsigned MutableExtraNonceSize_ = 4;
  double MinShareDiff_ = 10000.0;
};
