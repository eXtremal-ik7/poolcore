#ifndef __BACKEND_DATA_H_
#define __BACKEND_DATA_H_

#include "poolcore/poolCore.h"
#include "poolcore/workSummary.h"
#include "poolcommon/baseBlob.h"
#include "poolcommon/serialize.h"
#include "poolcommon/tagged.h"
#include "poolcommon/uint.h"
#include <list>
#include <string>
#include <vector>
#include <filesystem>
#include "p2putils/xmstream.h"

std::string partByHeight(uint64_t height);
std::string partByTime(time_t time);

typedef bool CheckAddressProcTy(const char*);

struct CMergedBlockInfo {
  std::string CoinName;
  uint64_t Height;
  std::string Hash;
};

struct CBlockFoundData {
  std::string UserId;
  uint64_t Height;
  std::string Hash;
  Timestamp Time;
  UInt<384> GeneratedCoins;
  UInt<256> ExpectedWork;
  uint32_t PrimePOWTarget;
  BaseBlob<256> ShareHash;
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

struct CNodeConfig {
  std::string Type;
  std::string Address;
  std::string Login;
  std::string Password;
  std::string Wallet;
  bool LongPollEnabled;
};

struct PoolBackendConfig {
  bool isMaster;
  std::filesystem::path dbPath;
  std::chrono::seconds ShareLogFlushInterval = std::chrono::seconds(3);
  uint64_t ShareLogFileSizeLimit = 4194304;

  unsigned RequiredConfirmations;
  UInt<384> DefaultPayoutThreshold;

  unsigned KeepStatsTime;
  unsigned ConfirmationsCheckInterval;
  unsigned BalanceCheckInterval;
  std::chrono::minutes StatisticKeepTime = std::chrono::minutes(30);
  std::chrono::minutes StatisticWorkersPowerCalculateInterval = std::chrono::minutes(10);
  std::chrono::minutes StatisticPoolPowerCalculateInterval = std::chrono::minutes(4);
  std::chrono::minutes StatisticPoolGridInterval = std::chrono::minutes(1);
  std::chrono::minutes StatisticUserGridInterval = std::chrono::minutes(3);
  std::chrono::minutes StatisticWorkerGridInterval = std::chrono::minutes(5);
  std::chrono::minutes StatisticPoolFlushInterval = std::chrono::minutes(1);
  std::chrono::minutes StatisticUserFlushInterval = std::chrono::minutes(3);
  std::chrono::minutes StatisticWorkerFlushInterval = std::chrono::minutes(5);
  std::chrono::minutes AccountingPPLNSWindow = std::chrono::minutes(30);
  std::chrono::minutes PPSPayoutInterval = std::chrono::minutes(5);
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
  Timestamp Time;
  UInt<384> Value;
  std::string TransactionId;
  std::string TransactionData;
  uint32_t Status = EInitialized;
  UInt<384> TxFee = UInt<384>::zero();
  double RateToBTC = 0.0;
  double RateBTCToUSD = 0.0;

  std::string getPartitionId() const { return partByTime(Time.toUnixTime()); }
  bool deserializeValue(const void *data, size_t size);
  bool deserializeValue(xmstream &stream);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;

  PayoutDbRecord() {}
  PayoutDbRecord(const std::string &userId, const UInt<384> &value) : UserId(userId), Time(Timestamp::now()), Value(value) {}
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

  CBlockFoundData Block;
  Timestamp StartTime;

  // Sum of PPLNS-adjusted work of all users
  UInt<256> TotalShareValue;
  // Block reward available for PPLNS distribution (reduced by PPS correction)
  UInt<384> AvailableForPPLNS;
  // Total work accumulated during the block search session
  UInt<256> AccumulatedWork;
  UInt<384> TxFee = UInt<384>::zero();

  std::vector<UserShareValue> UserShares;
  std::vector<CUserPayout> Payouts;

  // PPS correction applied at block discovery (non-deferred coins).
  // Stored so the deduction can be reversed if the block is orphaned.
  UInt<384> PPSValue;
  double PPSBlockPart = 0.0;

  MiningRound() {}
  MiningRound(unsigned heightArg) { Block.Height = heightArg; }

  friend bool operator<(const MiningRound &L, const MiningRound &R) { return L.Block.Height < R.Block.Height; }

  std::string getPartitionId() const { return partByTime(Block.Time.toUnixTime()); }
  bool deserializeValue(const void *data, size_t size);
  bool deserializeValue(xmstream &stream);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
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

struct CUserFeeConfig {
  std::string CoinName;
  std::vector<UserFeePair> Config;
};

struct CModeFeeConfig {
  std::vector<UserFeePair> Default;
  std::vector<CUserFeeConfig> CoinSpecific;
};

struct UserFeePlanRecord {
  std::string FeePlanId;
  // Indexed by EMiningMode
  std::vector<CModeFeeConfig> Modes;
  BaseBlob<256> ReferralId;

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

struct CSettingsPayout {
  EPayoutMode Mode = EPayoutMode::Disabled;
  std::string Address;
  UInt<384> InstantPayoutThreshold;

  static constexpr auto schema() {
    return std::make_tuple(
      field<1, &CSettingsPayout::Mode>(),
      field<2, &CSettingsPayout::Address>(),
      field<3, &CSettingsPayout::InstantPayoutThreshold>()
    );
  }
};

struct CSettingsMining {
  EMiningMode MiningMode = EMiningMode::PPLNS;

  static constexpr auto schema() {
    return std::make_tuple(
      field<1, &CSettingsMining::MiningMode>()
    );
  }
};

struct CSettingsAutoExchange {
  std::string PayoutCoinName;

  static constexpr auto schema() {
    return std::make_tuple(
      field<1, &CSettingsAutoExchange::PayoutCoinName>()
    );
  }
};

struct UserSettingsRecord : CSerializable<UserSettingsRecord> {
  std::string Login;
  std::string Coin;
  CSettingsPayout Payout;
  CSettingsMining Mining;
  CSettingsAutoExchange AutoExchange;

  UserSettingsRecord() {}
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;

  static constexpr auto schema() {
    return std::make_tuple(
      field<1, &UserSettingsRecord::Login>(),
      field<2, &UserSettingsRecord::Coin>(),
      field<3, &UserSettingsRecord::Payout>(),
      field<4, &UserSettingsRecord::Mining>(),
      field<5, &UserSettingsRecord::AutoExchange>()
    );
  }
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
  UInt<384> PPSPaid;

  UserBalanceRecord() {}
  UserBalanceRecord(const std::string &userIdArg, const UInt<384>&) :
    Login(userIdArg),
    Balance(UInt<384>::zero()),
    Requested(UInt<384>::zero()),
    Paid(UInt<384>::zero()),
    PPSPaid(UInt<384>::zero()) {}
      
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};


struct FoundBlockRecord {
  enum { CurrentRecordVersion = 1 };

  uint64_t Height;
  std::string Hash;
  Timestamp Time;
  UInt<384> GeneratedCoins;
  std::string FoundBy;
  // Expected work to find a block, calculated from block difficulty (e.g. nBits header field)
  UInt<256> ExpectedWork = UInt<256>::zero();
  // Total work accumulated during the block search session
  UInt<256> AccumulatedWork = UInt<256>::zero();
  std::string PublicHash;
  // Other blocks found by the same share in merged mining
  std::vector<CMergedBlockInfo> MergedBlocks;
  // Hash of the previous found block (forms a chain for efficient confirmation queries)
  std::string PrevFoundHash;
  // Share hash used to correlate merged mining blocks
  BaseBlob<256> ShareHash;

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

struct CPPLNSPayout {
  enum { CurrentRecordVersion = 1 };
  // Key part
  std::string Login;
  Timestamp RoundStartTime;
  std::string BlockHash;
  // Value part
  uint64_t BlockHeight;
  Timestamp RoundEndTime;
  UInt<384> PayoutValue;
  double RateToBTC;
  double RateBTCToUSD;

  std::string getPartitionId() const { return partByTime(RoundStartTime.toUnixTime()); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct CPPSPayout {
  enum { CurrentRecordVersion = 1 };
  // Key
  std::string Login;
  Timestamp IntervalBegin;
  // Value
  Timestamp IntervalEnd;
  UInt<384> PayoutValue;
  double RateToBTC;
  double RateBTCToUSD;

  std::string getPartitionId() const { return partByTime(IntervalBegin.toUnixTime()); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

template<>
struct DbIo<CMergedBlockInfo> {
  static inline void serialize(xmstream &stream, const CMergedBlockInfo &data) {
    DbIo<std::string>::serialize(stream, data.CoinName);
    DbIo<uint64_t>::serialize(stream, data.Height);
    DbIo<std::string>::serialize(stream, data.Hash);
  }

  static inline void unserialize(xmstream &stream, CMergedBlockInfo &data) {
    DbIo<std::string>::unserialize(stream, data.CoinName);
    DbIo<uint64_t>::unserialize(stream, data.Height);
    DbIo<std::string>::unserialize(stream, data.Hash);
  }
};

template<>
struct DbIo<CBlockFoundData> {
  static inline void serialize(xmstream &stream, const CBlockFoundData &data) {
    DbIo<std::string>::serialize(stream, data.UserId);
    DbIo<uint64_t>::serialize(stream, data.Height);
    DbIo<std::string>::serialize(stream, data.Hash);
    DbIo<Timestamp>::serialize(stream, data.Time);
    DbIo<UInt<384>>::serialize(stream, data.GeneratedCoins);
    DbIo<UInt<256>>::serialize(stream, data.ExpectedWork);
    DbIo<uint32_t>::serialize(stream, data.PrimePOWTarget);
    DbIo<BaseBlob<256>>::serialize(stream, data.ShareHash);
  }

  static inline void unserialize(xmstream &stream, CBlockFoundData &data) {
    DbIo<std::string>::unserialize(stream, data.UserId);
    DbIo<uint64_t>::unserialize(stream, data.Height);
    DbIo<std::string>::unserialize(stream, data.Hash);
    DbIo<Timestamp>::unserialize(stream, data.Time);
    DbIo<UInt<384>>::unserialize(stream, data.GeneratedCoins);
    DbIo<UInt<256>>::unserialize(stream, data.ExpectedWork);
    DbIo<uint32_t>::unserialize(stream, data.PrimePOWTarget);
    DbIo<BaseBlob<256>>::unserialize(stream, data.ShareHash);
  }
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
