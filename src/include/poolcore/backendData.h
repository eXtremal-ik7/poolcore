#ifndef __BACKEND_DATA_H_
#define __BACKEND_DATA_H_

#include "poolcommon/serialize.h"
#include "poolcommon/uint256.h"
#include <list>
#include <string>
#include <vector>
#include <filesystem>
#include "p2putils/xmstream.h"

std::string partByHeight(uint64_t height);
std::string partByTime(time_t time);

typedef bool CheckAddressProcTy(const char*);

struct CShare {
  uint64_t UniqueShareId = 0;
  std::string userId;
  std::string workerId;
  int64_t height;
  double WorkValue;
  bool isBlock;
  std::string hash;
  int64_t generatedCoins;
  int64_t Time;
  double ExpectedWork = 0.0;
};

struct PoolFeeEntry {
  std::string User;
  double Percentage;
};

template<typename T>
class SelectorByWeight {
public:
  void add(const T &value, uint32_t weight) {
    Values.push_back(value);
    ValueIndexes.insert(ValueIndexes.end(), weight, Values.size()-1);
  }

  const T &get() const { return Values[ValueIndexes[rand() % ValueIndexes.size()]]; }

private:
  struct Entry {
    T Value;
    uint32_t Weight;
  };

private:
  std::vector<T> Values;
  std::vector<size_t> ValueIndexes;
};

struct PoolBackendConfig {
  bool isMaster;
  std::filesystem::path dbPath;
  std::chrono::seconds ShareLogFlushInterval = std::chrono::seconds(3);
  uint64_t ShareLogFileSizeLimit = 134217728;

  std::vector<PoolFeeEntry> PoolFee;
  unsigned RequiredConfirmations;
  int64_t DefaultPayoutThreshold;
  int64_t MinimalAllowedPayout;
  unsigned KeepRoundTime;
  unsigned KeepStatsTime;
  unsigned ConfirmationsCheckInterval;
  unsigned PayoutInterval;
  unsigned BalanceCheckInterval;
  std::chrono::minutes StatisticKeepTime = std::chrono::minutes(30);
  std::chrono::minutes StatisticWorkersPowerCalculateInterval = std::chrono::minutes(11);
  std::chrono::minutes StatisticPoolPowerCalculateInterval = std::chrono::minutes(5);
  std::chrono::minutes StatisticWorkersAggregateTime = std::chrono::minutes(5);
  std::chrono::minutes StatisticPoolAggregateTime = std::chrono::minutes(1);
  std::chrono::hours StatisticKeepWorkerNamesTime = std::chrono::hours(24);

  SelectorByWeight<std::string> MiningAddresses;
  std::string CoinBaseMsg;

  // ZEC specify
  std::string poolTAddr;
  std::string poolZAddr;
};

struct roundElement {
  std::string userId;
  double shareValue;
};

struct PayoutDbRecord {
  enum { CurrentRecordVersion = 1 };
  enum EStatus {
    EInitialized = 0,
    ETxCreated,
    ETxSent,
    ETxConfirmed,
    ETxRejected
  };

  std::string UserId;
  int64_t Time;
  int64_t Value;
  std::string TransactionId;
  std::string TransactionData;
  uint32_t Status = EInitialized;

  std::string getPartitionId() const { return partByTime(Time); }
  bool deserializeValue(const void *data, size_t size);
  bool deserializeValue(xmstream &stream);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;

  PayoutDbRecord() {}
  PayoutDbRecord(const std::string &userId2, int64_t value2) : UserId(userId2), Value(value2) {}
};

struct shareInfo {
  std::string type;
  int64_t count;
};

struct miningRound {
  static constexpr uint32_t CurrentRecordVersion = 1;
  
  uint64_t height;
  std::string blockHash;
  time_t time;    
    
  // aggregated share and payment value
  double totalShareValue;
  int64_t availableCoins;
    
  std::list<roundElement> rounds;
  std::list<PayoutDbRecord> payouts;
    
  miningRound() {}
  miningRound(unsigned heightArg) : height(heightArg) {}
    
  friend bool operator<(const miningRound &L, const miningRound &R) { return L.height < R.height; }

  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
  void dump();
};

struct UsersRecord {
  enum { CurrentRecordVersion = 1 };

  std::string Login;
  std::string EMail;
  std::string Name;
  std::string TwoFactorAuthData;
  uint256 PasswordHash;
  int64_t RegistrationDate;
  bool IsActive;
  bool IsReadOnly = false;

  UsersRecord() {}
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct UserActionRecord {
  enum { CurrentRecordVersion = 1 };

  enum EType {
    UserActivate = 0,
    UserChangePassword,
    UserChangeEmail
  };

  uint512 Id;
  std::string Login;
  uint32_t Type;
  uint64_t CreationDate;

  UserActionRecord() {}
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct UserSessionRecord {
  enum { CurrentRecordVersion = 1 };

  uint512 Id;
  std::string Login;
  uint64_t LastAccessTime;
  bool Dirty = false;
  bool IsReadOnly = false;

  UserSessionRecord() {}
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;

  void updateLastAccessTime(uint64_t time) {
    LastAccessTime = time;
    Dirty = true;
  }
};

struct UserSettingsRecord {
  enum { CurrentRecordVersion = 1 };

  std::string Login;
  std::string Coin;
  std::string Address;
  int64_t MinimalPayout;
  bool AutoPayout;

  UserSettingsRecord() {}
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct FixedPointInteger {
public:
  FixedPointInteger() {}
  FixedPointInteger(int64_t value) : Value_(value) {}
  int64_t get() const { return Value_; }
  int64_t getRational(int64_t multiplier) const { return Value_ / multiplier; }

  void set(int64_t value) { Value_ = value; }

  void add(int64_t value) { Value_ += value; }
  void addRational(int64_t value, int64_t multiplier) { Value_ += value*multiplier; }
  void sub(int64_t value) { Value_ -= value; }
  void subRational(int64_t value, int64_t multiplier) { Value_ -= value*multiplier; }

private:
  int64_t Value_;
};

struct UserBalanceRecord {
  enum { CurrentRecordVersion = 1 };
  
  std::string Login;
  FixedPointInteger Balance;
  int64_t Requested;
  int64_t Paid;

  UserBalanceRecord() {}
  UserBalanceRecord(const std::string &userIdArg, int64_t) :
    Login(userIdArg), Balance(0), Requested(0), Paid(0) {}
      
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};


struct FoundBlockRecord {
  enum { CurrentRecordVersion = 1 };
  
  uint64_t Height;
  std::string Hash;
  int64_t Time;
  int64_t AvailableCoins;
  std::string FoundBy;
  double ExpectedWork = 0.0;
  double AccumulatedWork = 0.0;
  
  std::string getPartitionId() const { return partByHeight(Height); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct PoolBalanceRecord {
  enum { CurrentRecordVersion = 1 };
  
  int64_t Time;
  int64_t Balance;
  int64_t Immature;
  int64_t Users;
  int64_t Queued;
  int64_t Net;

  std::string getPartitionId() const { return partByTime(Time); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct StatsRecord {
  enum { CurrentRecordVersion = 1 };
  
  std::string Login;
  std::string WorkerId;
  int64_t Time;
  uint64_t ShareCount;
  double ShareWork;
  
  std::string getPartitionId() const { return partByTime(Time); }
  bool deserializeValue(xmstream &stream);
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct ShareStatsRecord {
  enum { CurrentRecordVersion = 1 };

  int64_t Time;
  int64_t Total;
  std::vector<shareInfo> Info;
  
  std::string getPartitionId() const { return partByTime(Time); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

template<>
struct DbIo<roundElement> {
  static inline void serialize(xmstream &stream, const roundElement &data) {
    DbIo<decltype (data.userId)>::serialize(stream, data.userId);
    DbIo<decltype (data.shareValue)>::serialize(stream, data.shareValue);
  }

  static inline void unserialize(xmstream &stream, roundElement &data) {
    DbIo<decltype (data.userId)>::unserialize(stream, data.userId);
    DbIo<decltype (data.shareValue)>::unserialize(stream, data.shareValue);
  }
};

#endif //__BACKEND_DATA_H_
