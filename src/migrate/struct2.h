#pragma once

#include "poolcommon/serialize.h"
#include "poolcore/poolCore.h"
#include <map>
#include <string>
#include <vector>

// Old database structures (database version 2)

struct UserSettingsRecord2 {
  enum { CurrentRecordVersion = 1 };

  std::string Login;
  std::string Coin;
  std::string Address;
  int64_t MinimalPayout;
  bool AutoPayout;

  UserSettingsRecord2() {}
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct StatsRecord2 {
  enum { CurrentRecordVersion = 1 };

  std::string Login;
  std::string WorkerId;
  int64_t Time;
  uint64_t ShareCount;
  double ShareWork;
  uint32_t PrimePOWTarget;
  std::vector<uint64_t> PrimePOWShareCount;

  StatsRecord2() {}
  bool deserializeValue(const void *data, size_t size);
};

struct FoundBlockRecord2 {
  enum { CurrentRecordVersion = 1 };

  uint64_t Height;
  std::string Hash;
  int64_t Time;
  int64_t AvailableCoins;
  std::string FoundBy;
  double ExpectedWork;
  double AccumulatedWork;
  std::string PublicHash;

  FoundBlockRecord2() {}
  bool deserializeValue(const void *data, size_t size);
};

struct CPPLNSPayout2 {
  enum { CurrentRecordVersion = 1 };

  std::string Login;
  int64_t RoundStartTime;
  std::string BlockHash;
  uint64_t BlockHeight;
  int64_t RoundEndTime;
  int64_t PayoutValue;
  int64_t PayoutValueWithoutFee;
  double AcceptedWork;
  uint32_t PrimePOWTarget;
  double RateToBTC;
  double RateBTCToUSD;

  CPPLNSPayout2() {}
  bool deserializeValue(const void *data, size_t size);
};

struct PayoutDbRecord2 {
  enum { CurrentRecordVersion = 2 };

  std::string UserId;
  int64_t Time;
  int64_t Value;
  std::string TransactionId;
  std::string TransactionData;
  uint32_t Status;
  int64_t TxFee;

  PayoutDbRecord2() {}
  bool deserializeValue(const void *data, size_t size);
  bool deserializeValue(xmstream &stream);
};

struct PoolBalanceRecord2 {
  enum { CurrentRecordVersion = 1 };

  int64_t Time;
  int64_t BalanceWithFractional;
  int64_t Immature;
  int64_t Users;
  int64_t Queued;
  int64_t ConfirmationWait;
  int64_t Net;

  PoolBalanceRecord2() {}
  bool deserializeValue(const void *data, size_t size);
};

struct UsersRecord2 {
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

  UsersRecord2() {}
  bool deserializeValue(const void *data, size_t size);
};

struct UserActionRecord2 {
  enum { CurrentRecordVersion = 1 };

  BaseBlob<512> Id;
  std::string Login;
  uint32_t Type;
  uint64_t CreationDate;
  std::string TwoFactorKey;

  UserActionRecord2() {}
  bool deserializeValue(const void *data, size_t size);
};

struct UserSessionRecord2 {
  enum { CurrentRecordVersion = 1 };

  BaseBlob<512> Id;
  std::string Login;
  uint64_t LastAccessTime;
  bool IsReadOnly = false;
  bool IsPermanent = false;

  UserSessionRecord2() {}
  bool deserializeValue(const void *data, size_t size);
};

// Old sub-structures for MiningRound2

struct UserShareValue2 {
  std::string UserId;
  double ShareValue;
  double IncomingWork;
  UserShareValue2() {}
  UserShareValue2(const std::string &userId, double shareValue, double incomingWork) :
    UserId(userId), ShareValue(shareValue), IncomingWork(incomingWork) {}
};

template<>
struct DbIo<UserShareValue2> {
  static inline void unserialize(xmstream &stream, UserShareValue2 &data) {
    DbIo<std::string>::unserialize(stream, data.UserId);
    DbIo<double>::unserialize(stream, data.ShareValue);
    DbIo<double>::unserialize(stream, data.IncomingWork);
  }
};

struct CUserPayout2 {
  std::string UserId;
  int64_t Value;
  int64_t ValueWithoutFee;
  double AcceptedWork;
  CUserPayout2() {}
  CUserPayout2(const std::string &userId, int64_t value, int64_t valueWithoutFee, double acceptedWork) :
    UserId(userId), Value(value), ValueWithoutFee(valueWithoutFee), AcceptedWork(acceptedWork) {}
};

template<>
struct DbIo<CUserPayout2> {
  static inline void unserialize(xmstream &stream, CUserPayout2 &data) {
    DbIo<std::string>::unserialize(stream, data.UserId);
    DbIo<int64_t>::unserialize(stream, data.Value);
    DbIo<int64_t>::unserialize(stream, data.ValueWithoutFee);
    DbIo<double>::unserialize(stream, data.AcceptedWork);
  }
};

template<>
struct DbIo<PayoutDbRecord2> {
  static inline void unserialize(xmstream &stream, PayoutDbRecord2 &data) {
    data.deserializeValue(stream);
  }
};

struct MiningRound2 {
  uint64_t Height;
  std::string BlockHash;
  int64_t EndTime;
  int64_t StartTime;
  double TotalShareValue;
  int64_t AvailableCoins;
  std::vector<UserShareValue2> UserShares;
  std::vector<CUserPayout2> Payouts;
  std::string FoundBy;
  double ExpectedWork;
  double AccumulatedWork;
  int64_t TxFee;
  uint32_t PrimePOWTarget;

  MiningRound2() {}
  bool deserializeValue(const void *data, size_t size);
};

struct UserBalanceRecord2 {
  enum { CurrentRecordVersion = 1 };

  std::string Login;
  int64_t BalanceWithFractional;
  int64_t Requested;
  int64_t Paid;

  UserBalanceRecord2() {}
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

// Old statistics cache structures (version 2)

struct CStatsFileRecordOld2 {
  enum { CurrentRecordVersion = 1 };

  std::string Login;
  std::string WorkerId;
  int64_t Time;
  uint64_t ShareCount;
  double ShareWork;
  uint32_t PrimePOWTarget;
  std::vector<uint64_t> PrimePOWShareCount;
};

struct CStatsFileDataOld2 {
  enum { CurrentRecordVersion = 1 };

  uint64_t LastShareId;
  std::vector<CStatsFileRecordOld2> Records;
};

struct CSharesWorkWithTimeOld2 {
  int64_t TimeLabel;
  double SharesWork;
};

struct CStatsExportDataOld2 {
  enum { CurrentRecordVersion = 1 };

  std::string UserId;
  std::vector<CSharesWorkWithTimeOld2> Recent;
};

// Old accounting file data (version 2)

struct CAccountingFileDataOld2 {
  enum { CurrentRecordVersion = 1 };

  uint64_t LastShareId;
  int64_t LastBlockTime;
  std::vector<CStatsExportDataOld2> Recent;
  std::map<std::string, double> CurrentScores;
};

// Old coin library (version 2) — only migration-specific fields
class CCoinLibraryOld2 {
public:
  static CCoinInfoOld2 get(const char *coinName);
};

// Parsed share for migration (common format for v0 and v1)

struct ParsedShare {
  uint64_t UniqueShareId = 0;
  std::string UserId;
  std::string WorkerId;
  double WorkValue = 0.0;
  int64_t Time = 0; // unix seconds
  uint32_t ChainLength = 0;
  uint32_t PrimePOWTarget = 0;
};

// Old share format (version 1)

struct CShareV1 {
  enum { CurrentRecordVersion = 1 };
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
  uint32_t ChainLength;
  uint32_t PrimePOWTarget;
};

// Forward declaration of ShareLogIo template (defined in poolcore/shareLog.h)
template<typename T>
struct ShareLogIo;

template<>
struct ShareLogIo<CShareV1> {
  static void serialize(xmstream &out, const CShareV1 &data);
  static void unserialize(xmstream &in, CShareV1 &data);
};

// DbIo specializations for old structures

template<>
struct DbIo<CStatsFileRecordOld2> {
  static inline void serialize(xmstream &out, const CStatsFileRecordOld2 &data) {
    DbIo<uint32_t>::serialize(out, data.CurrentRecordVersion);
    DbIo<decltype(data.Login)>::serialize(out, data.Login);
    DbIo<decltype(data.WorkerId)>::serialize(out, data.WorkerId);
    DbIo<decltype(data.Time)>::serialize(out, data.Time);
    DbIo<decltype(data.ShareCount)>::serialize(out, data.ShareCount);
    DbIo<decltype(data.ShareWork)>::serialize(out, data.ShareWork);
    DbIo<decltype(data.PrimePOWTarget)>::serialize(out, data.PrimePOWTarget);
    DbIo<decltype(data.PrimePOWShareCount)>::serialize(out, data.PrimePOWShareCount);
  }

  static inline void unserialize(xmstream &in, CStatsFileRecordOld2 &data) {
    uint32_t version;
    DbIo<uint32_t>::unserialize(in, version);
    if (version == 1) {
      DbIo<decltype(data.Login)>::unserialize(in, data.Login);
      DbIo<decltype(data.WorkerId)>::unserialize(in, data.WorkerId);
      DbIo<decltype(data.Time)>::unserialize(in, data.Time);
      DbIo<decltype(data.ShareCount)>::unserialize(in, data.ShareCount);
      DbIo<decltype(data.ShareWork)>::unserialize(in, data.ShareWork);
      DbIo<decltype(data.PrimePOWTarget)>::unserialize(in, data.PrimePOWTarget);
      DbIo<decltype(data.PrimePOWShareCount)>::unserialize(in, data.PrimePOWShareCount);
    } else {
      in.seekEnd(0, true);
    }
  }
};

template<>
struct DbIo<CSharesWorkWithTimeOld2> {
  static inline void serialize(xmstream &out, const CSharesWorkWithTimeOld2 &data) {
    DbIo<decltype(data.SharesWork)>::serialize(out, data.SharesWork);
    DbIo<decltype(data.TimeLabel)>::serialize(out, data.TimeLabel);
  }

  static inline void unserialize(xmstream &in, CSharesWorkWithTimeOld2 &data) {
    DbIo<decltype(data.SharesWork)>::unserialize(in, data.SharesWork);
    DbIo<decltype(data.TimeLabel)>::unserialize(in, data.TimeLabel);
  }
};

template<>
struct DbIo<CStatsExportDataOld2> {
  static inline void serialize(xmstream &out, const CStatsExportDataOld2 &data) {
    DbIo<uint32_t>::serialize(out, data.CurrentRecordVersion);
    DbIo<decltype(data.UserId)>::serialize(out, data.UserId);
    DbIo<decltype(data.Recent)>::serialize(out, data.Recent);
  }

  static inline void unserialize(xmstream &in, CStatsExportDataOld2 &data) {
    uint32_t version;
    DbIo<uint32_t>::unserialize(in, version);
    if (version == 1) {
      DbIo<decltype(data.UserId)>::unserialize(in, data.UserId);
      DbIo<decltype(data.Recent)>::unserialize(in, data.Recent);
    } else {
      in.seekEnd(0, true);
    }
  }
};

template<>
struct DbIo<CStatsFileDataOld2> {
  static inline void unserialize(xmstream &in, CStatsFileDataOld2 &data) {
    uint32_t version;
    DbIo<uint32_t>::unserialize(in, version);
    if (version == 1) {
      DbIo<decltype(data.LastShareId)>::unserialize(in, data.LastShareId);
      while (in.remaining()) {
        CStatsFileRecordOld2 &record = data.Records.emplace_back();
        DbIo<CStatsFileRecordOld2>::unserialize(in, record);
      }
    } else {
      in.seekEnd(0, true);
    }
  }
};

template<>
struct DbIo<CAccountingFileDataOld2> {
  static inline void unserialize(xmstream &in, CAccountingFileDataOld2 &data) {
    uint32_t version;
    DbIo<uint32_t>::unserialize(in, version);
    if (version == 1) {
      DbIo<decltype(data.LastShareId)>::unserialize(in, data.LastShareId);
      DbIo<decltype(data.LastBlockTime)>::unserialize(in, data.LastBlockTime);
      DbIo<decltype(data.Recent)>::unserialize(in, data.Recent);
      DbIo<decltype(data.CurrentScores)>::unserialize(in, data.CurrentScores);
    } else {
      in.seekEnd(0, true);
    }
  }
};
