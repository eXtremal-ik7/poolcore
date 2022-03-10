#pragma once

#include "poolcommon/uint256.h"
#include <string>
#include <thread>
#include <vector>
#include "p2putils/xmstream.h"
#include "poolcore/thread.h"
#include <atomic>
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
    EBCH = 8,
    EECash = 16,
    EZAddr = 32,
    EEth = 64
  };

  enum EPowerUnitType {
    EHash = 0,
    ECPD
  };

  std::string Name;
  std::string FullName;
  int64_t RationalPartSize;
  int64_t ExtraMultiplier = 100;
  EAddressType PayoutAddressType;
  bool SegwitEnabled;
  EPowerUnitType PowerUnitType;
  int32_t PowerMultLog10;

  std::vector<uint8_t> PubkeyAddressPrefix;
  std::vector<uint8_t> ScriptAddressPrefix;
  std::string Bech32Prefix;

  uint16_t DefaultRpcPort;

  std::string Algorithm;
  std::string CoinGeckoName;
  double ProfitSwitchDefaultCoeff = 0.0;
  unsigned MinimalConfirmationsNumber = 6;
  bool HasExtendedFundRawTransaction = true;
  double WorkMultiplier = 4294967296.0;
  uint256 PowLimit;
  bool HasDagFile = false;
  bool BigEpoch = false;

  bool checkAddress(const std::string &address, EAddressType type) const;
  const char *getPowerUnitName() const;
  uint64_t calculateAveragePower(double work, uint64_t timeInterval) const;
};

class CNetworkClient {
public:
  using SumbitBlockCb = std::function<void(bool, uint32_t, const std::string&, const std::string&)>;

  enum EOperationStatus {
    EStatusOk = 0,
    EStatusNetworkError,
    EStatusProtocolError,
    EStatusTimeout,
    EStatusMethodNotFound,
    EStatusTooSmallOutputs,
    EStatusInsufficientFunds,
    EStatusVerifyRejected,
    EStatusInvalidAddressOrKey,
    EStatusUnknownError
  };

  struct GetBlockConfirmationsQuery {
    std::string Hash;
    uint64_t Height;
    int64_t Confirmations;

    GetBlockConfirmationsQuery() {}
    GetBlockConfirmationsQuery(const std::string &hash, uint64_t height) : Hash(hash), Height(height) {}
  };

  struct GetBlockExtraInfoQuery {
    // Input
    std::string Hash;
    uint64_t Height;
    // Input & output
    int64_t TxFee;
    int64_t BlockReward;
    // Output
    int64_t Confirmations;
    std::string PublicHash;

    GetBlockExtraInfoQuery() {}
    GetBlockExtraInfoQuery(const std::string &hash, uint64_t height, int64_t txFee, int64_t lastKnownBlockReward) :
      Hash(hash), Height(height), TxFee(txFee), BlockReward(lastKnownBlockReward) {}
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

  struct BuildTransactionResult {
    std::string TxId;
    std::string TxData;
    std::string Error;
    int64_t Value;
    int64_t Fee;
  };

  struct ListUnspentElement {
    std::string Address;
    int64_t Amount;
    bool IsCoinbase;
  };

  struct ListUnspentResult {
    std::vector<ListUnspentElement> Outs;
  };

  struct ZSendMoneyResult {
    std::string AsyncOperationId;
    std::string Error;
  };

  class CSubmitBlockOperation {
  public:
    CSubmitBlockOperation(CNetworkClient::SumbitBlockCb callback, size_t clientsNum) : Callback_(callback), ClientsNum_(clientsNum) {}
    void accept(bool result, const std::string &hostName, const std::string &error);
  private:
    CNetworkClient::SumbitBlockCb Callback_;
    size_t ClientsNum_;
    std::atomic<uint32_t> State_ = 0;
  };

public:
  CNetworkClient(unsigned threadsNum) {
    ThreadData_.reset(new ThreadData[threadsNum]);
  }

  virtual ~CNetworkClient() {}

  virtual CPreparedQuery *prepareBlock(const void *data, size_t size) = 0;
  virtual bool ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockConfirmationsQuery> &query) = 0;
  virtual bool ioGetBlockExtraInfo(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockExtraInfoQuery> &query) = 0;
  virtual bool ioGetBalance(asyncBase *base, GetBalanceResult &result) = 0;
  virtual EOperationStatus ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const int64_t value, BuildTransactionResult &result) = 0;
  virtual EOperationStatus ioSendTransaction(asyncBase *base, const std::string &txData, std::string &error) = 0;
  virtual EOperationStatus ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, std::string &error) = 0;
  virtual EOperationStatus ioListUnspent(asyncBase *base, ListUnspentResult &result) = 0;
  virtual EOperationStatus ioZSendMany(asyncBase *base, const std::string &source, const std::string &destination, int64_t amount, const std::string &memo, uint64_t minConf, int64_t fee, CNetworkClient::ZSendMoneyResult &result) = 0;
  virtual EOperationStatus ioZGetBalance(asyncBase *base, const std::string &address, int64_t *balance) = 0;
  virtual void aioSubmitBlock(asyncBase *base, CPreparedQuery *query, CSubmitBlockOperation *operation) = 0;

  virtual void poll() = 0;

  void setDispatcher(CNetworkClientDispatcher *dispatcher) { Dispatcher_ = dispatcher; }

protected:
  CNetworkClientDispatcher *Dispatcher_ = nullptr;

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

