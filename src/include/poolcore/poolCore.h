#pragma once

#include "poolcommon/periodicTimer.h"
#include "poolcommon/uint.h"
#include "poolcore/feeEstimator.h"
#include "workTypes.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace loguru { class LogChannel; }
class CBlockTemplate;
class CPoolInstance;
class PoolBackend;
struct CNetworkState;
struct aioUserEvent;
struct asyncBase;

struct CCoinInfoOld2 {
  int64_t ExtraMultiplier = 100;
  double WorkMultiplier = 4294967296.0;
};

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
  bool IsAlgorithm = false;
  unsigned FractionalPartSize;
  EAddressType PayoutAddressType;
  bool SegwitEnabled;
  bool MWebEnabled = false;
  EPowerUnitType PowerUnitType;
  int32_t PowerMultLog10;

  std::vector<uint8_t> PubkeyAddressPrefix;
  std::vector<uint8_t> ScriptAddressPrefix;
  std::string Bech32Prefix;
  uint8_t PrivateKeyPrefix = 0;

  uint16_t DefaultRpcPort;

  std::string Algorithm;
  std::string CoinGeckoName;
  std::string NodeRepository;
  double ProfitSwitchDefaultCoeff = 0.0;
  unsigned MinimalConfirmationsNumber = 6;
  bool HasExtendedFundRawTransaction = true;
  UInt<256> PowLimit;
  // ETH configuration
  bool HasDagFile = false;
  bool BigEpoch = false;
  // XEC delayed blocks
  bool HasRtt = false;
  // Deferred reward (exact value unknown at template time, needs estimation)
  bool HasDeferredReward = false;
  // Merged mining configuration
  EWorkType WorkType;
  bool CanBePrimaryCoin = true;
  bool CanBeSecondaryCoin = false;
  bool ResetWorkOnBlockChange = true;

  bool PPSIncludeTransactionFees = true;

  std::chrono::seconds WorkSummaryFlushInterval = std::chrono::seconds(6);

  // Default minimal payout thresholds
  UInt<384> DefaultInstantMinimalPayout;
  UInt<384> DefaultRegularMinimalPayout;

  bool checkAddress(const std::string &address, EAddressType type) const;
  const char *getPowerUnitName() const;
  uint64_t calculateAveragePower(const UInt<256> &work, uint64_t timeInterval, unsigned int primePOWTarget) const;
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
    UInt<384> TxFee;
    UInt<384> BlockReward;
    // Output
    int64_t Confirmations;
    std::string PublicHash;

    GetBlockExtraInfoQuery() {}
    GetBlockExtraInfoQuery(const std::string &hash, uint64_t height, const UInt<384> &txFee, const UInt<384> &lastKnownBlockReward) :
      Hash(hash), Height(height), TxFee(txFee), BlockReward(lastKnownBlockReward) {}
  };

  struct GetBalanceResult {
    UInt<384> Balance;
    UInt<384> Immatured;
  };

  struct BuildTransactionResult {
    std::string TxId;
    std::string TxData;
    std::string Error;
    UInt<384> Value;
    UInt<384> Fee;
  };

  struct ListUnspentElement {
    std::string Address;
    UInt<384> Amount;
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
  virtual ~CNetworkClient() = default;

  virtual bool ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockConfirmationsQuery> &query) = 0;
  virtual bool ioGetBlockExtraInfo(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockExtraInfoQuery> &query) = 0;
  virtual bool ioGetBalance(asyncBase *base, GetBalanceResult &result) = 0;
  virtual EOperationStatus ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const UInt<384> &value, BuildTransactionResult &result) = 0;
  virtual EOperationStatus ioSendTransaction(asyncBase *base, const std::string &txData, const std::string &txId, std::string &error) = 0;
  virtual EOperationStatus ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, UInt<384> *txFee, std::string &error) = 0;
  virtual EOperationStatus ioListUnspent(asyncBase *base, ListUnspentResult &result) = 0;
  virtual EOperationStatus ioZSendMany(asyncBase *base, const std::string &source, const std::string &destination, const UInt<384> &amount, const std::string &memo, uint64_t minConf, const UInt<384> &fee, CNetworkClient::ZSendMoneyResult &result) = 0;
  virtual EOperationStatus ioZGetBalance(asyncBase *base, const std::string &address, UInt<384> *balance) = 0;
  virtual EOperationStatus ioWalletService(asyncBase *base, std::string &error) = 0;
  void aioSubmitBlock(asyncBase *base, const void *data, size_t size, SumbitBlockCb callback);

  virtual void poll() = 0;

  // --- Multi-node infrastructure (used by typed multi-node subclasses) ---
  void connectWith(CPoolInstance *instance);
  void setBackend(PoolBackend *backend) { Backend_ = backend; }
  PoolBackend *backend() { return Backend_; }
  void setNetworkState(std::atomic<CNetworkState> *networkState) { NetworkState_ = networkState; }

  void start(asyncBase *base, const CCoinInfo &coinInfo);
  void stop();
  virtual const UInt<384> &averageFee() const { return AverageFee_; }
  virtual UInt<384> estimatedBaseReward(int64_t) const { return UInt<384>::zero(); }

protected:
  virtual void aioSubmitBlock(asyncBase *base, const void *data, size_t size, CSubmitBlockOperation *operation) = 0;

  void reportNewWork(CBlockTemplate *blockTemplate);
  void reportConnectionLost();

  virtual size_t getWorkClientsNum() const = 0;
  virtual void updateFeeEstimation() {}
  virtual void doAdvanceWorkFetcher() {}

  asyncBase *Base_ = nullptr;
  CCoinInfo CoinInfo_;
  PoolBackend *Backend_ = nullptr;
  std::atomic<CNetworkState> *NetworkState_ = nullptr;
  loguru::LogChannel *LogChannel_ = nullptr;
  std::vector<CPoolInstance*> LinkedInstances_;

  // Fee estimation state (accessible to subclass updateFeeEstimation overrides)
  std::unique_ptr<CFeeEstimator> Estimator_;
  int64_t LastBlockHeight_ = 0;
  int64_t PendingHeight_ = 0;
  UInt<384> AverageFee_;

private:
  void onReconnectTimer();

  enum EWorkState { EWorkOk = 0, EWorkLost, EWorkMinersStopped };

  aioUserEvent *ReconnectTimer_ = nullptr;
  EWorkState WorkState_ = EWorkOk;
  bool Stopped_ = false;
  std::chrono::time_point<std::chrono::steady_clock> ConnectionLostTime_;
  std::unique_ptr<CPeriodicTimer> FeeUpdateTimer_;
};

template<typename Container, typename Function>
auto roundRobin(Container &clients, std::atomic<size_t> &currentIndex, Function &&function)
  -> decltype(function(*clients[0]))
{
  using R = decltype(function(*clients[0]));
  R lastResult;
  if constexpr (std::is_same_v<R, bool>)
    lastResult = false;
  else
    lastResult = CNetworkClient::EStatusUnknownError;

  size_t start = currentIndex.load(std::memory_order_relaxed);
  for (size_t i = 0, n = clients.size(); i < n; ++i) {
    size_t index = (start + i) % n;
    lastResult = function(*clients[index]);
    bool success;
    if constexpr (std::is_same_v<R, bool>)
      success = lastResult;
    else
      success = (lastResult == CNetworkClient::EStatusOk);
    if (success)
      return lastResult;
    currentIndex.store((index + 1) % n, std::memory_order_relaxed);
  }
  return lastResult;
}

