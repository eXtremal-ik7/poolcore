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


static inline void addId(JSON::Object &object, StratumMessage &msg) {
  if (!msg.stringId.empty())
    object.addString("id", msg.stringId);
  else
    object.addInt("id", msg.integerId);
}

template<typename X>
class StratumInstance : public CPoolInstance {
public:
  StratumInstance(asyncBase *monitorBase, UserManager &userMgr, CThreadPool &threadPool, rapidjson::Value &config) : CPoolInstance(monitorBase, userMgr, threadPool), CurrentThreadId_(0) {
    Name_ = (std::string)X::Proto::TickerName + ".stratum";
    Data_.reset(new ThreadData[threadPool.threadsNum()]);
    for (unsigned i = 0; i < threadPool.threadsNum(); i++) {
      X::Stratum::initializeThreadConfig(Data_[i].ThreadCfg, i, threadPool.threadsNum());
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

    if (config.HasMember("mergedMiningEnabled")  && config["mergedMiningEnabled"].IsTrue())
      MergedMiningEnabled_ = true;

    X::Stratum::initializeMiningConfig(MiningCfg_, config);

    // Main listener
    createListener(monitorBase, port, [](socketTy socket, HostAddress address, void *arg) { static_cast<StratumInstance*>(arg)->newFrontendConnection(socket, address); }, this);
  }

  virtual void checkNewBlockTemplate(CBlockTemplate *blockTemplate, PoolBackend *backend) override {
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, blockTemplate, backend));
  }

  virtual void stopWork() override {
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, nullptr, nullptr));
  }

  void acceptWork(unsigned, CBlockTemplate *blockTemplate, PoolBackend *backend) {
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
      } else {
        // Deep copy last work (required for merged mining)
        typename X::Stratum::Work *clone = new typename X::Stratum::Work(*data.WorkSet.back());
        data.WorkSet.emplace_back(clone);
      }
    } else {
      isNewBlock = true;
      data.WorkSet.emplace_back(new typename X::Stratum::Work);
    }

    // Get mining address and coinbase message
    typename X::Stratum::Work &work = *data.WorkSet.back();
    typename X::Proto::AddressTy miningAddress;
    if (!decodeHumanReadableAddress(backendConfig.MiningAddress, coinInfo.PubkeyAddressPrefix, miningAddress)) {
      LOG_F(WARNING, "%s: mining address %s is invalid", coinInfo.Name.c_str(), backendConfig.MiningAddress.c_str());
      return;
    }

    std::string error;
    if (!X::Stratum::loadFromTemplate(work, blockTemplate->Document, blockTemplate->UniqueWorkId, MiningCfg_, backend, coinInfo.Name, miningAddress, backendConfig.CoinBaseMsg, error)) {
      LOG_F(ERROR, "%s: can't process block template; error: %s", Name_.c_str(), error.c_str());
      return;
    }

    if (isNewBlock) {
      data.KnownShares.clear();
      data.KnownBlocks.clear();
      // Update major job id
      uint64_t newJobId = time(nullptr);
      data.MajorWorkId_ = (data.MajorWorkId_ == newJobId) ? newJobId+1 : newJobId;
    }

    if (work.initialized()) {
      X::Stratum::buildNotifyMessage(work, MiningCfg_, data.MajorWorkId_, data.WorkSet.size() - 1, isNewBlock, work.NotifyMessage);
      for (auto &connection: data.Connections_) {
        stratumSendWork(connection);
      }
    }
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
    std::unordered_set<std::string> KnownShares;
    std::unordered_set<std::string> KnownBlocks;
    std::vector<std::unique_ptr<typename X::Stratum::Work>> WorkSet;
    std::set<Connection*> Connections_;
    uint64_t MajorWorkId_ = 0;

    // Merged mining data
    typename X::Stratum::Work WorkAcc_;
  };

private:
  void onStratumSubscribe(Connection *connection, StratumMessage &msg) {
    char buffer[4096];
    xmstream stream(buffer, sizeof(buffer));
    stream.reset();
    X::Stratum::onSubscribe(MiningCfg_, connection->WorkerConfig, msg, stream);
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

  bool shareCheck(Connection *connection, StratumMessage &msg) {
    ThreadData &data = Data_[GetLocalThreadId()];

    // Check worker name
    auto It = connection->Workers.find(msg.submit.WorkerName);
    if (It == connection->Workers.end())
      return false;
    Worker &worker = It->second;

    // Check job id
    size_t jobIndex;
    {
      size_t sharpPos = msg.submit.JobId.find('#');
      if (sharpPos == msg.submit.JobId.npos)
        return false;

      msg.submit.JobId[sharpPos] = 0;
      uint64_t majorJobId = xatoi<uint64_t>(msg.submit.JobId.c_str());
      jobIndex = xatoi<size_t>(msg.submit.JobId.c_str() + sharpPos + 1);
      if (majorJobId != data.MajorWorkId_ || jobIndex >= data.WorkSet.size())
        return false;
    }

    // Build header and check proof of work
    std::string user = worker.User;
    typename X::Stratum::Work &work = *data.WorkSet[jobIndex];
    X::Stratum::prepareForSubmit(work, connection->WorkerConfig, MiningCfg_, msg);

    // Loop over all affected backends (usually 1 or 2 with enabled merged mining)
    bool shareAccepted = false;
    for (size_t i = 0, ie = work.backendsNum(); i != ie; ++i) {
      PoolBackend *backend = work.backend(i);
      if (!backend)
        continue;

      uint64_t height = work.height(i);
      double shareDiff = 0.0;
      bool isBlock = work.checkConsensus(i, &shareDiff);
      if (shareDiff < connection->ShareDifficulty)
        continue;

      shareAccepted = true;
      if (isBlock) {
        std::string blockHash = work.hash(i);
        LOG_F(INFO, "%s: new proof of work for %s found; hash: %s; transactions: %zu", Name_.c_str(), backend->getCoinInfo().Name.c_str(), blockHash.c_str(), work.txNum(i));

        // Serialize block
        int64_t generatedCoins = work.blockReward(i);
        CNetworkClientDispatcher &dispatcher = backend->getClientDispatcher();
        dispatcher.aioSubmitBlock(data.WorkerBase, work.blockHexData(i).data(), work.blockHexData(i).sizeOf(), [height, user, blockHash, generatedCoins, backend, &data](bool result, const std::string &hostName, const std::string &error) {
          if (result) {
            LOG_F(INFO, "* block %s (%" PRIu64 ") accepted by %s", blockHash.c_str(), height, hostName.c_str());
            if (data.KnownBlocks.insert(blockHash).second) {
              // Send share with block to backend
              CAccountingShare *backendShare = new CAccountingShare;
              backendShare->userId = user;
              backendShare->height = height;
              // TODO: calculate this value
              backendShare->value = 1;
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
        CAccountingShare *backendShare = new CAccountingShare;
        backendShare->userId = worker.User;
        backendShare->height = height;
        // TODO: calculate this value
        backendShare->value = 1;
        backendShare->isBlock = false;
        backend->sendShare(backendShare);
      }
    }

    return shareAccepted;
  }

  void onStratumSubmit(Connection *connection, StratumMessage &msg) {
    bool result = shareCheck(connection, msg);
    xmstream stream;
    {
      // Response
      JSON::Object object(stream);
      addId(object, msg);
      object.addBoolean("result", result);
      object.addNull("error");
    }

    stream.write('\n');
    aioWrite(connection->Socket, stream.data(), stream.sizeOf(), afWaitAll, 0, nullptr, nullptr);
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
        params.addDouble(connection->ShareDifficulty);
      }
    }

    stream.write('\n');
    aioWrite(connection->Socket, stream.data(), stream.sizeOf(), afWaitAll, 0, nullptr, nullptr);
    {
      // TODO: WARNING -> DEBUG
      std::string text(stream.data<const char>(), stream.sizeOf());
      LOG_F(WARNING, "%s", text.c_str());
    }
  }

  void stratumSendWork(Connection *connection) {
    ThreadData &data = Data_[GetLocalThreadId()];
    typename X::Stratum::Work &work = *data.WorkSet.back();
    {
      // TODO: WARNING -> DEBUG
      std::string text(static_cast<const char*>(work.NotifyMessage.data()), work.NotifyMessage.sizeOf());
      LOG_F(WARNING, "%s", text.c_str());
    }
    aioWrite(connection->Socket, work.NotifyMessage.data(), work.NotifyMessage.sizeOf(), afWaitAll, 0, nullptr, nullptr);
  }

  void newFrontendConnection(socketTy fd, HostAddress address) {
    ThreadData &data = Data_[CurrentThreadId_];
    Connection *connection = new Connection(this, newSocketIo(data.WorkerBase, fd), CurrentThreadId_, address);
    X::Stratum::initializeWorkerConfig(connection->WorkerConfig, data.ThreadCfg);

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
              if (result && !data.WorkSet.empty()) {
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
  unsigned CurrentThreadId_;
  typename X::Stratum::MiningConfig MiningCfg_;
  double ConstantShareDiff_;

  // Merged mining data
  bool MergedMiningEnabled_ = false;
};
