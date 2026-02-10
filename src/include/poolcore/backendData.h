#ifndef __BACKEND_DATA_H_
#define __BACKEND_DATA_H_

#include "poolcommon/baseBlob.h"
#include "poolcommon/serialize.h"
#include "poolcommon/uint.h"
#include <list>
#include <string>
#include <vector>
#include <filesystem>
#include "p2putils/xmstream.h"

std::string partByHeight(uint64_t height);
std::string partByTime(time_t time);

typedef bool CheckAddressProcTy(const char*);

struct CShare {
  enum { CurrentRecordVersion = 1 };
  uint64_t UniqueShareId = 0;
  std::string userId;
  std::string workerId;
  int64_t height;
  UInt<256> WorkValue;
  bool isBlock;
  std::string hash;
  UInt<384> generatedCoins;
  Timestamp Time;
  UInt<256> ExpectedWork = UInt<256>::zero();
  uint32_t ChainLength;
  uint32_t PrimePOWTarget;
};

struct CMiningAddress {
  std::string MiningAddress;
  std::string PrivateKey;
  CMiningAddress() {}
  CMiningAddress(const std::string &miningAddress, const std::string &privateKey) : MiningAddress(miningAddress), PrivateKey(privateKey) {}
};

template<typename T>
class SelectorByWeight {
public:
  void add(const T &value, uint32_t weight) {
    Values.push_back(value);
    ValueIndexes.insert(ValueIndexes.end(), weight, Values.size()-1);
  }

  size_t size() const { return Values.size(); }
  const T &get() const {
    static T empty;
    return ValueIndexes.size() ? Values[ValueIndexes[rand() % ValueIndexes.size()]] : empty;
  }
  const T &getByIndex(size_t index) const { return Values[index]; }

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
  uint64_t ShareLogFileSizeLimit = 4194304;

  unsigned RequiredConfirmations;
  UInt<384> DefaultPayoutThreshold;
  UInt<384> MinimalAllowedPayout;
  unsigned KeepRoundTime;
  unsigned KeepStatsTime;
  unsigned ConfirmationsCheckInterval;
  unsigned PayoutInterval;
  unsigned BalanceCheckInterval;
  std::chrono::minutes StatisticKeepTime = std::chrono::minutes(30);
  std::chrono::minutes StatisticWorkersPowerCalculateInterval = std::chrono::minutes(11);
  std::chrono::minutes StatisticPoolPowerCalculateInterval = std::chrono::minutes(5);
  std::chrono::seconds StatisticPoolGridInterval = std::chrono::seconds(60);
  std::chrono::seconds StatisticUserGridInterval = std::chrono::seconds(120);
  std::chrono::seconds StatisticWorkerGridInterval = std::chrono::seconds(300);
  std::chrono::seconds StatisticFlushInterval = std::chrono::seconds(60);
  std::chrono::hours StatisticKeepWorkerNamesTime = std::chrono::hours(24);

  SelectorByWeight<CMiningAddress> MiningAddresses;
  std::string CoinBaseMsg;

  // ZEC specific
  std::string poolTAddr;
  std::string poolZAddr;
};

struct UserShareValue {
  std::string UserId;
  // PPLNS-adjusted work (work for the last block search session + work for a defined interval, e.g. last hour)
  UInt<256> ShareValue;
  // Work accumulated only during the last block search session
  UInt<256> IncomingWork;
  UserShareValue() {}
  UserShareValue(const std::string &userId, UInt<256> shareValue, UInt<256> incomingWork) :
    UserId(userId), ShareValue(shareValue), IncomingWork(incomingWork) {}
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
  UInt<384> Value;
  std::string TransactionId;
  std::string TransactionData;
  uint32_t Status = EInitialized;
  // Version 2
  UInt<384> TxFee = UInt<384>::zero();

  std::string getPartitionId() const { return partByTime(Time); }
  bool deserializeValue(const void *data, size_t size);
  bool deserializeValue(xmstream &stream);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;

  PayoutDbRecord() {}
  PayoutDbRecord(const std::string &userId, const UInt<384> &value) : UserId(userId), Value(value) {}
};

struct shareInfo {
  std::string type;
  int64_t count;
};

struct CUserPayout {
  std::string UserId;
  UInt<384> Value;
  UInt<384> ValueWithoutFee;
  UInt<256> AcceptedWork;
  CUserPayout() {}
  CUserPayout(const std::string &userId, const UInt<384> &value, const UInt<384> &valueWithoutFee, const UInt<256> &acceptedWork) :
    UserId(userId), Value(value), ValueWithoutFee(valueWithoutFee), AcceptedWork(acceptedWork) {}
};

struct MiningRound {
  static constexpr uint32_t CurrentRecordVersion = 1;
  
  uint64_t Height;
  std::string BlockHash;
  int64_t EndTime;
  int64_t StartTime;
    
  // Sum of PPLNS-adjusted work of all users
  UInt<256> TotalShareValue;
  // Full block reward
  UInt<384> AvailableCoins;

  // ETH specific
  // ===
  std::string FoundBy;
  // Expected work to find a block, calculated from block difficulty (e.g. nBits header field)
  UInt<256> ExpectedWork;
  // Total work accumulated during the block search session
  UInt<256> AccumulatedWork;
  UInt<384> TxFee = UInt<384>::zero();

  // XPM specific
  // ===
  uint32_t PrimePOWTarget;

  std::vector<UserShareValue> UserShares;
  std::vector<CUserPayout> Payouts;
    
  MiningRound() {}
  MiningRound(unsigned heightArg) : Height(heightArg) {}
    
  friend bool operator<(const MiningRound &L, const MiningRound &R) { return L.Height < R.Height; }

  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
  void dump(unsigned fractionalPart);
};

struct UsersRecord {
  enum { CurrentRecordVersion = 1 };

  std::string Login;
  std::string EMail;
  std::string Name;
  std::string TwoFactorAuthData;
  std::string ParentUser;
  BaseBlob<256> PasswordHash;
  int64_t RegistrationDate;
  bool IsActive;
  bool IsReadOnly = false;
  bool IsSuperUser = false;
  std::string FeePlanId;
  std::string MonitoringSessionId;

  UsersRecord() {}
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct CoinSpecificFeeRecord {
  std::string CoinName;
  double Fee;
  CoinSpecificFeeRecord() {}
  CoinSpecificFeeRecord(const std::string &coinName, double fee) : CoinName(coinName), Fee(fee) {}
};

struct UserPersonalFeeRecord {
  enum { CurrentRecordVersion = 1 };

  std::string UserId;
  std::string ParentUserId;
  double DefaultFee;
  std::vector<CoinSpecificFeeRecord> CoinSpecificFee;

  UserPersonalFeeRecord() {}
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct UserFeePair {
  std::string UserId;
  double Percentage;
};

using UserFeeConfig = std::vector<UserFeePair>;

struct CoinSpecificFeeRecord2 {
  std::string CoinName;
  UserFeeConfig Config;
};

struct UserFeePlanRecord {
  enum { CurrentRecordVersion = 1 };

  std::string FeePlanId;
  UserFeeConfig Default;
  std::vector<CoinSpecificFeeRecord2> CoinSpecificFee;

  UserFeePlanRecord() {}
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
    UserChangeEmail,
    UserTwoFactorActivate,
    UserTwoFactorDeactivate
  };

  BaseBlob<512> Id;
  std::string Login;
  uint32_t Type;
  uint64_t CreationDate;
  std::string TwoFactorKey;

  UserActionRecord() {}
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct UserSessionRecord {
  enum { CurrentRecordVersion = 1 };

  BaseBlob<512> Id;
  std::string Login;
  uint64_t LastAccessTime;
  bool Dirty = false;
  bool IsReadOnly = false;
  bool IsPermanent = false;

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
  UInt<384> MinimalPayout;
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
  UInt<384> Balance;
  UInt<384> Requested;
  UInt<384> Paid;

  UserBalanceRecord() {}
  UserBalanceRecord(const std::string &userIdArg, const UInt<384>&) :
    Login(userIdArg), Balance(UInt<384>::zero()), Requested(UInt<384>::zero()), Paid(UInt<384>::zero()) {}
      
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
  UInt<384> AvailableCoins;
  std::string FoundBy;
  // Expected work to find a block, calculated from block difficulty (e.g. nBits header field)
  UInt<256> ExpectedWork = UInt<256>::zero();
  // Total work accumulated during the block search session
  UInt<256> AccumulatedWork = UInt<256>::zero();
  std::string PublicHash;
  
  std::string getPartitionId() const { return partByHeight(Height); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct PoolBalanceRecord {
  enum { CurrentRecordVersion = 1 };
  
  int64_t Time;
  UInt<384> Balance;
  UInt<384> Immature;
  UInt<384> Users;
  UInt<384> Queued;
  UInt<384> ConfirmationWait;
  UInt<384> Net;

  std::string getPartitionId() const { return partByTime(Time); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct StatsRecord {
  enum { CurrentRecordVersion = 1 };

  std::string Login;
  std::string WorkerId;
  TimeInterval Time;
  Timestamp UpdateTime;
  uint64_t ShareCount;
  UInt<256> ShareWork;
  uint32_t PrimePOWTarget;
  std::vector<uint64_t> PrimePOWShareCount;

  std::string getPartitionId() const { return partByTime(Time.TimeEnd.toUnixTime()); }
  bool deserializeValue(xmstream &stream);
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct CPPLNSPayout {
  enum { CurrentRecordVersion = 1 };
  // Key part
  std::string Login;
  int64_t RoundStartTime;
  std::string BlockHash;
  // Value part
  uint64_t BlockHeight;
  int64_t RoundEndTime;
  UInt<384> PayoutValue;
  UInt<384> PayoutValueWithoutFee;
  UInt<256> AcceptedWork;
  uint32_t PrimePOWTarget;
  double RateToBTC;
  double RateBTCToUSD;

  std::string getPartitionId() const { return partByTime(RoundStartTime); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

template<>
struct DbIo<UserShareValue> {
  static inline void serialize(xmstream &stream, const UserShareValue &data) {
    DbIo<decltype (data.UserId)>::serialize(stream, data.UserId);
    DbIo<decltype (data.ShareValue)>::serialize(stream, data.ShareValue);
    DbIo<decltype (data.IncomingWork)>::serialize(stream, data.IncomingWork);
  }

  static inline void unserialize(xmstream &stream, UserShareValue &data) {
    DbIo<decltype (data.UserId)>::unserialize(stream, data.UserId);
    DbIo<decltype (data.ShareValue)>::unserialize(stream, data.ShareValue);
    DbIo<decltype (data.IncomingWork)>::unserialize(stream, data.IncomingWork);
  }
};

template<>
struct DbIo<CUserPayout> {
  static inline void serialize(xmstream &stream, const CUserPayout &data) {
    DbIo<decltype (data.UserId)>::serialize(stream, data.UserId);
    DbIo<decltype (data.Value)>::serialize(stream, data.Value);
    DbIo<decltype (data.ValueWithoutFee)>::serialize(stream, data.ValueWithoutFee);
    DbIo<decltype (data.AcceptedWork)>::serialize(stream, data.AcceptedWork);
  }

  static inline void unserialize(xmstream &stream, CUserPayout &data) {
    DbIo<decltype (data.UserId)>::unserialize(stream, data.UserId);
    DbIo<decltype (data.Value)>::unserialize(stream, data.Value);
    DbIo<decltype (data.ValueWithoutFee)>::unserialize(stream, data.ValueWithoutFee);
    DbIo<decltype (data.AcceptedWork)>::unserialize(stream, data.AcceptedWork);
  }
};

// For backward compatibility
struct UserShareValue1 {
  std::string userId;
  double shareValue;
  UserShareValue1() {}
  UserShareValue1(const std::string &userId_, double shareValue_) : userId(userId_), shareValue(shareValue_) {}
};

template<>
struct DbIo<UserShareValue1> {
  static inline void serialize(xmstream &stream, const UserShareValue1 &data) {
    DbIo<decltype (data.userId)>::serialize(stream, data.userId);
    DbIo<decltype (data.shareValue)>::serialize(stream, data.shareValue);
  }

  static inline void unserialize(xmstream &stream, UserShareValue1 &data) {
    DbIo<decltype (data.userId)>::unserialize(stream, data.userId);
    DbIo<decltype (data.shareValue)>::unserialize(stream, data.shareValue);
  }
};

#endif //__BACKEND_DATA_H_
