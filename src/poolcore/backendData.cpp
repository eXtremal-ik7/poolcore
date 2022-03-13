#include "poolcore/backendData.h"
#include "poolcommon/serialize.h"
#include <inttypes.h>
#include <time.h>

template<typename T>
void dbIoSerialize(xmstream &dst, const T &data) { DbIo<T>::serialize(dst, data); }
template<typename T>
void dbIoUnserialize(xmstream &src, T &data) { DbIo<T>::unserialize(src, data); }

template<typename T>
void dbKeyIoSerialize(xmstream &dst, const T &data) { DbKeyIo<T>::serialize(dst, data); }

std::string partByHeight(uint64_t height)
{
  char buffer[16];
  xitoa<uint64_t>(height / 1000000, buffer);
  return buffer;
}

std::string partByTime(time_t time)
{
  char buffer[16];
  tm *utc = gmtime(&time);
  if (utc)
    sprintf(buffer, "%04u.%02u", utc->tm_year+1900, utc->tm_mon+1);
  else
    strcpy(buffer, "2999.12");
  return buffer;
}

template<>
struct DbIo<PayoutDbRecord> {
  static inline void serialize(xmstream &stream, const PayoutDbRecord &data) {
    dbIoSerialize(stream, static_cast<uint32_t>(data.CurrentRecordVersion));
    dbIoSerialize(stream, data.UserId);
    dbIoSerialize(stream, data.Time);
    dbIoSerialize(stream, data.Value);
    dbIoSerialize(stream, data.TransactionId);
    dbIoSerialize(stream, data.TransactionData);
    dbIoSerialize(stream, data.Status);
    dbIoSerialize(stream, data.TxFee);
  }

  static inline void unserialize(xmstream &stream, PayoutDbRecord &data) {
    uint32_t version;
    dbIoUnserialize(stream, version);
    if (version >= 1) {
      dbIoUnserialize(stream, data.UserId);
      dbIoUnserialize(stream, data.Time);
      dbIoUnserialize(stream, data.Value);
      dbIoUnserialize(stream, data.TransactionId);
      dbIoUnserialize(stream, data.TransactionData);
      dbIoUnserialize(stream, data.Status);
      if (version >= 2) {
        dbIoUnserialize(stream, data.TxFee);
      }
    }
  }
};

template<>
struct DbIo<CoinSpecificFeeRecord> {
  static inline void serialize(xmstream &stream, const CoinSpecificFeeRecord &data) {
    dbIoSerialize(stream, data.CoinName);
    dbIoSerialize(stream, data.Fee);
  }

  static inline void unserialize(xmstream &stream, CoinSpecificFeeRecord &data) {
    dbIoUnserialize(stream, data.CoinName);
    dbIoUnserialize(stream, data.Fee);
  }
};

template<>
struct DbIo<UserFeePair> {
  static inline void serialize(xmstream &stream, const UserFeePair &data) {
    dbIoSerialize(stream, data.UserId);
    dbIoSerialize(stream, data.Percentage);
  }

  static inline void unserialize(xmstream &stream, UserFeePair &data) {
    dbIoUnserialize(stream, data.UserId);
    dbIoUnserialize(stream, data.Percentage);
  }
};

template<>
struct DbIo<CoinSpecificFeeRecord2> {
  static inline void serialize(xmstream &stream, const CoinSpecificFeeRecord2 &data) {
    dbIoSerialize(stream, data.CoinName);
    dbIoSerialize(stream, data.Config);
  }

  static inline void unserialize(xmstream &stream, CoinSpecificFeeRecord2 &data) {
    dbIoUnserialize(stream, data.CoinName);
    dbIoUnserialize(stream, data.Config);
  }
};

void MiningRound::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, Height);
  dbKeyIoSerialize(stream, BlockHash);
}

void MiningRound::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, Height);
  dbIoSerialize(stream, BlockHash);
  dbIoSerialize(stream, Time);
  dbIoSerialize(stream, TotalShareValue);
  dbIoSerialize(stream, AvailableCoins);
  dbIoSerialize(stream, UserShares);
  dbIoSerialize(stream, Payouts);
  dbIoSerialize(stream, FoundBy);
  dbIoSerialize(stream, ExpectedWork);
  dbIoSerialize(stream, AccumulatedWork);
  dbIoSerialize(stream, TxFee);
}

bool MiningRound::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);

  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, Height);
    dbIoUnserialize(stream, BlockHash);
    dbIoUnserialize(stream, Time);
    dbIoUnserialize(stream, TotalShareValue);
    dbIoUnserialize(stream, AvailableCoins);
    dbIoUnserialize(stream, UserShares);
    dbIoUnserialize(stream, Payouts);
    if (stream.remaining()) {
      dbIoUnserialize(stream, FoundBy);
      dbIoUnserialize(stream, ExpectedWork);
      dbIoUnserialize(stream, AccumulatedWork);
      dbIoUnserialize(stream, TxFee);
    }
  }
  
  return !stream.eof();
}

void MiningRound::dump()
{
  fprintf(stderr, "height=%u\n", (unsigned)Height);
  fprintf(stderr, "blockhash=%s\n", BlockHash.c_str());
  fprintf(stderr, "time=%u\n", (unsigned)Time);
  fprintf(stderr, "totalShareValue=%.3lf\n", TotalShareValue);
  fprintf(stderr, "availableCoins=%" PRId64 "\n", AvailableCoins);
  for (auto r: UserShares) {
    fprintf(stderr, " *** round element ***\n");
    fprintf(stderr, " * userId: %s\n", r.userId.c_str());
    fprintf(stderr, " * shareValue: %.3lf\n", r.shareValue);
  }
  for (auto p: Payouts) {
    fprintf(stderr, " *** payout element ***\n");
    fprintf(stderr, " * userId: %s\n", p.UserId.c_str());
    fprintf(stderr, " * payoutValue: %" PRId64 "\n", p.Value);
  }  
}

bool UsersRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, Login);
    dbIoUnserialize(stream, EMail);
    dbIoUnserialize(stream, Name);
    dbIoUnserialize(stream, ParentUser);
    dbIoUnserialize(stream, TwoFactorAuthData);
    dbIoUnserialize(stream, PasswordHash);
    dbIoUnserialize(stream, RegistrationDate);
    dbIoUnserialize(stream, IsActive);
    dbIoUnserialize(stream, IsReadOnly);
    dbIoUnserialize(stream, IsSuperUser);
    if (stream.remaining()) {
      dbIoUnserialize(stream, FeePlanId);
    }
  }

  return !stream.eof();
}

void UsersRecord::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, Login);
}

void UsersRecord::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, Login);
  dbIoSerialize(stream, EMail);
  dbIoSerialize(stream, Name);
  dbIoSerialize(stream, ParentUser);
  dbIoSerialize(stream, TwoFactorAuthData);
  dbIoSerialize(stream, PasswordHash);
  dbIoSerialize(stream, RegistrationDate);
  dbIoSerialize(stream, IsActive);
  dbIoSerialize(stream, IsReadOnly);
  dbIoSerialize(stream, IsSuperUser);
  dbIoSerialize(stream, FeePlanId);
}

bool UserSettingsRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, Login);
    dbIoUnserialize(stream, Coin);
    dbIoUnserialize(stream, Address);
    dbIoUnserialize(stream, MinimalPayout);
    dbIoUnserialize(stream, AutoPayout);
  }

  return !stream.eof();
}

void UserSettingsRecord::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, Login);
  dbKeyIoSerialize(stream, Coin);
}

void UserSettingsRecord::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, Login);
  dbIoSerialize(stream, Coin);
  dbIoSerialize(stream, Address);
  dbIoSerialize(stream, MinimalPayout);
  dbIoSerialize(stream, AutoPayout);
}

bool UserPersonalFeeRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version == 1) {
    dbIoUnserialize(stream, UserId);
    dbIoUnserialize(stream, ParentUserId);
    dbIoUnserialize(stream, DefaultFee);
    dbIoUnserialize(stream, CoinSpecificFee);
  }

  return !stream.eof();
}

void UserPersonalFeeRecord::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, UserId);
}

void UserPersonalFeeRecord::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, UserId);
  dbIoSerialize(stream, ParentUserId);
  dbIoSerialize(stream, DefaultFee);
  dbIoSerialize(stream, CoinSpecificFee);
}

bool UserFeePlanRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version == 1) {
    dbIoUnserialize(stream, FeePlanId);
    dbIoUnserialize(stream, Default);
    dbIoUnserialize(stream, CoinSpecificFee);
  }

  return !stream.eof();
}

void UserFeePlanRecord::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, FeePlanId);
}

void UserFeePlanRecord::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, FeePlanId);
  dbIoSerialize(stream, Default);
  dbIoSerialize(stream, CoinSpecificFee);
}

// UserActionRecord

bool UserActionRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, Id);
    dbIoUnserialize(stream, Login);
    dbIoUnserialize(stream, Type);
    dbIoUnserialize(stream, CreationDate);
    if (stream.remaining()) {
      dbIoUnserialize(stream, TwoFactorKey);
    }
  }

  return !stream.eof();
}

void UserActionRecord::serializeKey(xmstream &stream) const {
  dbKeyIoSerialize(stream, Id);
}

void UserActionRecord::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, Id);
  dbIoSerialize(stream, Login);
  dbIoSerialize(stream, Type);
  dbIoSerialize(stream, CreationDate);
  dbIoSerialize(stream, TwoFactorKey);
}

// UserSessionRecord

bool UserSessionRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, Id);
    dbIoUnserialize(stream, Login);
    dbIoUnserialize(stream, LastAccessTime);
    if (stream.remaining())
      dbIoUnserialize(stream, IsReadOnly);
  }

  return !stream.eof();
}

void UserSessionRecord::serializeKey(xmstream &stream) const {
  dbKeyIoSerialize(stream, Id);
}

void UserSessionRecord::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, Id);
  dbIoSerialize(stream, Login);
  dbIoSerialize(stream, LastAccessTime);
  dbIoSerialize(stream, IsReadOnly);
}

bool UserBalanceRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version;
  int64_t balance;
  dbIoUnserialize(stream, version);
  if (version >= 1) { 
    dbIoUnserialize(stream, Login);
    dbIoUnserialize(stream, balance);
    dbIoUnserialize(stream, Requested);
    dbIoUnserialize(stream, Paid);
    Balance.set(balance);
  }
  
  return !stream.eof();
}

// TODO: remove obsolete table
void UserBalanceRecord::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, Login);
}

// TODO: remove obsolete table
void UserBalanceRecord::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, Login);
  dbIoSerialize(stream, Balance.get());
  dbIoSerialize(stream, Requested);
  dbIoSerialize(stream, Paid);
}

// ====================== FoundBlock ======================

bool FoundBlockRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, Height);
    dbIoUnserialize(stream, Hash);
    dbIoUnserialize(stream, Time);
    dbIoUnserialize(stream, AvailableCoins);
    dbIoUnserialize(stream, FoundBy);
    if (stream.remaining()) {
      dbIoUnserialize(stream, ExpectedWork);
      dbIoUnserialize(stream, AccumulatedWork);
      if (stream.remaining()) {
        dbIoUnserialize(stream, PublicHash);
      }
    }
  }
  
  return !stream.eof();
}


void FoundBlockRecord::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, Height);
  dbKeyIoSerialize(stream, Hash);
}

void FoundBlockRecord::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, Height);
  dbIoSerialize(stream, Hash);
  dbIoSerialize(stream, Time);
  dbIoSerialize(stream, AvailableCoins);
  dbIoSerialize(stream, FoundBy);
  dbIoSerialize(stream, ExpectedWork);
  dbIoSerialize(stream, AccumulatedWork);
  dbIoSerialize(stream, PublicHash);
}

// ====================== PoolBalance ======================

bool PoolBalanceRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, Time);
    dbIoUnserialize(stream, Balance);
    dbIoUnserialize(stream, Immature);
    dbIoUnserialize(stream, Users);
    dbIoUnserialize(stream, Queued);
    dbIoUnserialize(stream, ConfirmationWait);
    dbIoUnserialize(stream, Net);
  }
  
  return !stream.eof();
}

void PoolBalanceRecord::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, Time);
}

void PoolBalanceRecord::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, Time);
  dbIoSerialize(stream, Balance);
  dbIoSerialize(stream, Immature);
  dbIoSerialize(stream, Users);
  dbIoSerialize(stream, Queued);
  dbIoSerialize(stream, ConfirmationWait);
  dbIoSerialize(stream, Net);
}

// ====================== ClientStatsRecord ======================

bool StatsRecord::deserializeValue(xmstream &stream)
{
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, Login);
    dbIoUnserialize(stream, WorkerId);
    dbIoUnserialize(stream, Time);
    dbIoUnserialize(stream, ShareCount);
    dbIoUnserialize(stream, ShareWork);
  }

  return !stream.eof();
}

bool StatsRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  return deserializeValue(stream);
}

void StatsRecord::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, Login);
  dbKeyIoSerialize(stream, WorkerId);
  dbKeyIoSerialize(stream, Time);
}

void StatsRecord::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, Login);
  dbIoSerialize(stream, WorkerId);
  dbIoSerialize(stream, Time);
  dbIoSerialize(stream, ShareCount);
  dbIoSerialize(stream, ShareWork);
}

// ====================== payoutRecord ======================

bool PayoutDbRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  return deserializeValue(stream);
}

bool PayoutDbRecord::deserializeValue(xmstream &stream)
{
  dbIoUnserialize(stream, *this);
  return !stream.eof();
}

void PayoutDbRecord::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, UserId);
  dbKeyIoSerialize(stream, Time);
  dbKeyIoSerialize(stream, TransactionId);
}

void PayoutDbRecord::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, *this);
}
