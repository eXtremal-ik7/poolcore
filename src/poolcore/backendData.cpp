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
    }
  }
};

void miningRound::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, height);
  dbKeyIoSerialize(stream, blockHash);
}

void miningRound::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, height);
  dbIoSerialize(stream, blockHash);
  dbIoSerialize(stream, time);
  dbIoSerialize(stream, totalShareValue);
  dbIoSerialize(stream, availableCoins);
  dbIoSerialize(stream, rounds);
  dbIoSerialize(stream, payouts);
}

bool miningRound::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);

  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, height);
    dbIoUnserialize(stream, blockHash);
    dbIoUnserialize(stream, time);
    dbIoUnserialize(stream, totalShareValue);
    dbIoUnserialize(stream, availableCoins);
    dbIoUnserialize(stream, rounds);
    dbIoUnserialize(stream, payouts);
  }
  
  return !stream.eof();
}

void miningRound::dump()
{
  fprintf(stderr, "height=%u\n", (unsigned)height);
  fprintf(stderr, "blockhash=%s\n", blockHash.c_str());
  fprintf(stderr, "time=%u\n", (unsigned)time);
  fprintf(stderr, "totalShareValue=%.3lf\n", totalShareValue);
  fprintf(stderr, "availableCoins=%" PRId64 "\n", availableCoins);
  for (auto r: rounds) {
    fprintf(stderr, " *** round element ***\n");
    fprintf(stderr, " * userId: %s\n", r.userId.c_str());
    fprintf(stderr, " * shareValue: %.3lf\n", r.shareValue);
  }
  for (auto p: payouts) {
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
