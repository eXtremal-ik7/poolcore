#pragma once

#include <tbb/concurrent_queue.h>
#include <string>
#include <thread>
#include <vector>
#include "rapidjson/document.h"
#include "p2putils/xmstream.h"
#include "poolcore/thread.h"
#include <functional>
#include <stack>

class CPreparedQuery;
class CNetworkClientDispatcher;
struct asyncBase;

struct CCoinInfo {
  enum EAddressType {
    EP2PKH = 1,
    EPS2H = 2,
    EBech32 = 4,
    EZAddr = 8
  };

  std::string Name;
  int64_t RationalPartSize;
  EAddressType PayoutAddressType;
  bool SegwitEnabled;

  std::vector<uint8_t> PubkeyAddressPrefix;
  std::vector<uint8_t> ScriptAddressPrefix;

  uint16_t DefaultRpcPort;

  bool checkAddress(const std::string &address, EAddressType type);
};

class CNetworkClient {
public:
  using SumbitBlockCb = std::function<void(bool, const std::string&, const std::string&)>;

  struct GetBlockConfirmationsQuery {
    std::string Hash;
    uint64_t Height;
    int64_t Confirmations;
    GetBlockConfirmationsQuery() {}
    GetBlockConfirmationsQuery(const std::string &hash, uint64_t height) : Hash(hash), Height(height) {}
  };

  struct GetBalanceResult {
    int64_t Balance;
    int64_t Immatured;
  };

  struct SendMoneyResult {
    std::string TxId;
    std::string Error;
    int64_t Fee;
  };

  struct ListUnspentElement {
    std::string Address;
    int64_t Amount;
  };

  struct ListUnspentResult {
    std::vector<ListUnspentElement> Outs;
  };

  struct ZSendMoneyResult {
    std::string AsyncOperationId;
    std::string Error;
  };

public:
  CNetworkClient(unsigned threadsNum) {
    ThreadData_.reset(new ThreadData[threadsNum]);
  }

  virtual ~CNetworkClient() {}

  virtual CPreparedQuery *prepareBlock(const void *data, size_t size) = 0;
  virtual bool ioGetBlockConfirmations(asyncBase *base, std::vector<GetBlockConfirmationsQuery> &query) = 0;
  virtual bool ioGetBalance(asyncBase *base, GetBalanceResult &result) = 0;
  virtual bool ioSendMoney(asyncBase *base, const char *address, int64_t value, SendMoneyResult &result) = 0;
  virtual void aioSubmitBlock(asyncBase *base, CPreparedQuery *query, SumbitBlockCb callback) = 0;

  virtual void poll() = 0;

  void setDispatcher(CNetworkClientDispatcher *dispatcher) { Dispatcher_ = dispatcher; }

protected:
  CNetworkClientDispatcher *Dispatcher_ = nullptr;

protected:


private:
  struct ThreadData {
    std::stack<std::unique_ptr<xmstream>> MemoryPool;
  };

private:
  std::unique_ptr<ThreadData[]> ThreadData_;

friend class CPreparedQuery;
};

class CPreparedQuery {
public:
  CPreparedQuery(CNetworkClient *client) : Client_(client) {
    auto &memoryPool = Client_->ThreadData_[GetGlobalThreadId()].MemoryPool;
    if (!memoryPool.empty()) {
      Stream_ = memoryPool.top().release();
      memoryPool.pop();
    } else {
      Stream_ = new xmstream;
    }
  }

  virtual ~CPreparedQuery() {
    Client_->ThreadData_[GetGlobalThreadId()].MemoryPool.emplace(Stream_);
  }

  template<typename T> T *client() { return static_cast<T*>(Client_); }
  xmstream &stream() { return *Stream_; }
  void setPayLoadOffset(size_t offset) { PayLoadOffset_ = offset; }
  size_t payLoadOffset() { return PayLoadOffset_; }

protected:
  CNetworkClient *Client_ = nullptr;
  xmstream *Stream_ = nullptr;
  size_t PayLoadOffset_ = 0;
};

