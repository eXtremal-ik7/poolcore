#define __STDC_FORMAT_MACROS
#include "poolcore/backendData.h"
#include <inttypes.h>
#include <time.h>

std::string partByHeight(unsigned height)
{
  char buffer[16];
  xitoa<unsigned>(height / 1000000, buffer);
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

static inline void serializeStringForKey(xmstream &stream, const std::string &S)
{
  stream.writebe<uint32_t>(S.size());
  stream.write(S.data(), S.size());
}

static inline void serializeString(xmstream &stream, const std::string &S)
{
  stream.write<uint32_t>(S.size());
  stream.write(S.data(), S.size());
}

template<unsigned int BITS>
static inline void serializeUInt(xmstream &stream, const base_blob<BITS> &data)
{
  stream.write(data.begin(), data.size());
}

static inline void deserializeString(xmstream &stream, std::string &S)
{
  size_t size = stream.read<uint32_t>();
  const char *data = stream.seek<const char>(size);
  if (data)
    S.assign(data, size);
}

template<unsigned int BITS>
static inline void deserializeUInt(xmstream &stream, base_blob<BITS> &data)
{
  stream.read(data.begin(), data.size());
}

bool payoutElement::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  return deserializeValue(stream);
}

bool payoutElement::deserializeValue(xmstream &stream)
{
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) {
    deserializeString(stream, Login);
    payoutValue = stream.read<int64_t>();
    queued = stream.read<int64_t>();
    deserializeString(stream, asyncOpId);
  }
  
  return !stream.eof();
}

void payoutElement::serializeValue(xmstream &stream) const
{
  stream.write(CurrentRecordVersion);
  serializeString(stream, Login);
  stream.write<int64_t>(payoutValue);
  stream.write<int64_t>(queued);
  serializeString(stream, asyncOpId);
}


void miningRound::serializeKey(xmstream &stream) const
{
  stream.writebe<uint32_t>(height);
  serializeStringForKey(stream, blockHash);
}

void miningRound::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);
  stream.write<uint32_t>(height);
  serializeString(stream, blockHash);
  stream.write<uint64_t>(time);
  stream.write<int64_t>(totalShareValue);
  stream.write<int64_t>(availableCoins);
  
  stream.write<uint32_t>(rounds.size());
  for (auto r: rounds) {
    serializeString(stream, r.userId);
    stream.write<int64_t>(r.shareValue);
  }
  
  stream.write<uint32_t>(payouts.size());
  for (auto p: payouts) {
    serializeString(stream, p.Login);
    stream.write<int64_t>(p.payoutValue);
    stream.write<int64_t>(p.queued);    
  }
}

bool miningRound::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) {
    height = stream.read<uint32_t>();
    deserializeString(stream, blockHash);
    time = (time_t)stream.read<uint64_t>();
    totalShareValue = stream.read<int64_t>();
    availableCoins = stream.read<int64_t>();
  
    {
      size_t roundsNum = stream.read<uint32_t>();
      if (stream.eof())
        return false;
      rounds.clear();
    
      for (size_t i = 0; i < roundsNum; i++) {
        roundElement element;
        deserializeString(stream, element.userId);
        element.shareValue = stream.read<int64_t>();      
        rounds.push_back(element);
      }
    }
  
    {
      size_t payoutsNum = stream.read<uint32_t>();
      if (stream.eof())
        return false;
      payouts.clear();
    
      for (size_t i = 0; i < payoutsNum; i++) {
        payoutElement element;
        deserializeString(stream, element.Login);
        element.payoutValue = stream.read<int64_t>();
        element.queued = stream.read<int64_t>();
        payouts.push_back(element);
      }
    }
  }
  
  return !stream.eof();
}

void miningRound::dump()
{
  fprintf(stderr, "height=%u\n", (unsigned)height);
  fprintf(stderr, "blockhash=%s\n", blockHash.c_str());
  fprintf(stderr, "time=%u\n", (unsigned)time);
  fprintf(stderr, "totalShareValue=%" PRId64 "\n", totalShareValue);
  fprintf(stderr, "availableCoins=%" PRId64 "\n", availableCoins);
  for (auto r: rounds) {
    fprintf(stderr, " *** round element ***\n");
    fprintf(stderr, " * userId: %s\n", r.userId.c_str());
    fprintf(stderr, " * shareValue: %" PRId64 "\n", r.shareValue);
  }
  for (auto p: payouts) {
    fprintf(stderr, " *** payout element ***\n");
    fprintf(stderr, " * userId: %s\n", p.Login.c_str());
    fprintf(stderr, " * payoutValue: %" PRId64 "\n", p.payoutValue);
    fprintf(stderr, " * queued: %" PRId64 "\n", p.queued);    
  }  
}

bool UsersRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version = stream.readle<uint32_t>();
  if (version >= 1) {
    deserializeString(stream, Login);
    deserializeString(stream, EMail);
    deserializeString(stream, Name);
    deserializeString(stream, TwoFactorAuthData);
    deserializeUInt(stream, PasswordHash);
    RegistrationDate = stream.readle<uint64_t>();
    IsActive = stream.read<uint8_t>();
  }

  return !stream.eof();
}

void UsersRecord::serializeKey(xmstream &stream) const
{
  serializeStringForKey(stream, Login);
}

void UsersRecord::serializeValue(xmstream &stream) const
{
  stream.writele<uint32_t>(CurrentRecordVersion);
  serializeString(stream, Login);
  serializeString(stream, EMail);
  serializeString(stream, Name);
  serializeString(stream, TwoFactorAuthData);
  serializeUInt(stream, PasswordHash);
  stream.write<uint64_t>(RegistrationDate);
  stream.write<uint8_t>(IsActive);
}

bool UserSettingsRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version = stream.readle<uint32_t>();
  if (version >= 1) {
    deserializeString(stream, Login);
    deserializeString(stream, Coin);
    deserializeString(stream, Address);
    MinimalPayout = stream.readle<int64_t>();
    AutoPayout = stream.read<uint8_t>();
  }

  return !stream.eof();
}

void UserSettingsRecord::serializeKey(xmstream &stream) const
{
  serializeStringForKey(stream, Login);
  serializeStringForKey(stream, Coin);
}

void UserSettingsRecord::serializeValue(xmstream &stream) const
{
  stream.writele<uint32_t>(CurrentRecordVersion);
  serializeString(stream, Login);
  serializeString(stream, Coin);
  serializeString(stream, Address);
  stream.writele<int64_t>(MinimalPayout);
  stream.write<uint8_t>(AutoPayout);
}

// UserActionRecord

bool UserActionRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version = stream.readle<uint32_t>();
  if (version >= 1) {
    deserializeUInt(stream, Id);
    deserializeString(stream, Login);
    Type = stream.readle<uint32_t>();
    CreationDate = stream.readle<uint64_t>();
  }

  return !stream.eof();
}

void UserActionRecord::serializeKey(xmstream &stream) const {
  serializeUInt(stream, Id);
}

void UserActionRecord::serializeValue(xmstream &stream) const
{
  stream.writele<uint32_t>(CurrentRecordVersion);
  serializeUInt(stream, Id);
  serializeString(stream, Login);
  stream.writele<uint32_t>(Type);
  stream.writele<uint64_t>(CreationDate);
}

// UserSessionRecord

bool UserSessionRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version = stream.readle<uint32_t>();
  if (version >= 1) {
    deserializeUInt(stream, Id);
    deserializeString(stream, Login);
    LastAccessTime = stream.readle<uint64_t>();
  }

  return !stream.eof();
}

void UserSessionRecord::serializeKey(xmstream &stream) const {
  serializeUInt(stream, Id);
}

void UserSessionRecord::serializeValue(xmstream &stream) const
{
  stream.writele<uint32_t>(CurrentRecordVersion);
  serializeUInt(stream, Id);
  serializeString(stream, Login);
  stream.writele<uint64_t>(LastAccessTime);
}

bool UserBalanceRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) { 
    deserializeString(stream, Login);
    Balance = stream.readle<int64_t>();
    Requested = stream.readle<int64_t>();
    Paid = stream.readle<int64_t>();
  }
  
  return !stream.eof();
}

// TODO: remove obsolete table
void UserBalanceRecord::serializeKey(xmstream &stream) const
{
  serializeStringForKey(stream, Login);
}

// TODO: remove obsolete table
void UserBalanceRecord::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);  
  serializeString(stream, Login);
  stream.writele<int64_t>(Balance);
  stream.writele<int64_t>(Requested);
  stream.writele<int64_t>(Paid);
}

// ====================== FoundBlock ======================

bool FoundBlockRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) {
    Height = stream.read<uint64_t>();
    deserializeString(stream, Hash);
    Time = stream.readle<uint64_t>();
    AvailableCoins = stream.read<int64_t>();
    deserializeString(stream, FoundBy);
  }
  
  return !stream.eof();
}


void FoundBlockRecord::serializeKey(xmstream &stream) const
{
  stream.writebe<uint64_t>(Height);
  serializeStringForKey(stream, Hash);
}

void FoundBlockRecord::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);
  stream.writele<uint64_t>(Height);
  serializeString(stream, Hash);
  stream.writele<uint64_t>(Time);
  stream.writele<int64_t>(AvailableCoins);
  serializeString(stream, FoundBy);
}

// ====================== PoolBalance ======================

bool PoolBalanceRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) {
    Time = (time_t)stream.read<uint64_t>();
    Balance = stream.read<int64_t>();
    Immature = stream.read<int64_t>();
    Users = stream.read<int64_t>();
    Queued = stream.read<int64_t>();
    Net = stream.read<int64_t>();
  }
  
  return !stream.eof();
}

void PoolBalanceRecord::serializeKey(xmstream &stream) const
{
  stream.writebe<uint64_t>(Time);
}

void PoolBalanceRecord::serializeValue(xmstream &stream) const
{
  stream.writele<uint32_t>(CurrentRecordVersion);
  stream.writele<uint64_t>(Time);
  stream.writele<int64_t>(Balance);
  stream.writele<int64_t>(Immature);
  stream.writele<int64_t>(Users);
  stream.writele<int64_t>(Queued);
  stream.writele<int64_t>(Net);
}

// ====================== SiteStats ======================

bool SiteStatsRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) {
    deserializeString(stream, Login);
    Time = (time_t)stream.read<uint64_t>();
    Clients = stream.read<uint32_t>();
    Workers = stream.read<uint32_t>();
    CPUNum = stream.read<uint32_t>();
    GPUNum = stream.read<uint32_t>();
    ASICNum = stream.read<uint32_t>();
    OtherNum = stream.read<uint32_t>();
    Latency = stream.read<uint32_t>();
    Power = stream.read<uint64_t>();
  }
  
  return !stream.eof();  
}

void SiteStatsRecord::serializeKey(xmstream &stream) const
{
  serializeStringForKey(stream, Login);
  stream.writebe<uint64_t>(Time);
}

void SiteStatsRecord::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);
  serializeString(stream, Login);
  stream.write<uint64_t>(Time);
  stream.write<uint32_t>(Clients);
  stream.write<uint32_t>(Workers);
  stream.write<uint32_t>(CPUNum);
  stream.write<uint32_t>(GPUNum);
  stream.write<uint32_t>(ASICNum);
  stream.write<uint32_t>(OtherNum);
  stream.write<uint32_t>(Latency);
  stream.write<uint64_t>(Power);
}

// ====================== ClientStatsRecord ======================

bool ClientStatsRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) {
    deserializeString(stream, Login);
    deserializeString(stream, WorkerId);
    Time = (time_t)stream.read<uint64_t>();
    Power = stream.read<uint64_t>();
    Latency = stream.read<int32_t>();
    deserializeString(stream, Address);
    UnitType = stream.read<int32_t>();
    Units = stream.read<uint32_t>();
    Temp = stream.read<uint32_t>();
  }
  
  return !stream.eof();    
}

void ClientStatsRecord::serializeKey(xmstream &stream) const
{
  serializeStringForKey(stream, Login);
  serializeStringForKey(stream, WorkerId);
  stream.writebe<uint64_t>(Time);
}

void ClientStatsRecord::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);
  serializeString(stream, Login);
  serializeString(stream, WorkerId);
  stream.write<uint64_t>(Time);
  stream.write<uint64_t>(Power);
  stream.write<int32_t>(Latency);
  serializeString(stream, Address);
  stream.write<int32_t>(UnitType);
  stream.write<uint32_t>(Units);
  stream.write<uint32_t>(Temp);
}

// ====================== ShareStatsRecord ======================

bool ShareStatsRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) {
    Time = (time_t)stream.read<uint64_t>();
    Total = stream.read<int64_t>();
    
    {
      Info.clear();
      unsigned size = stream.read<uint32_t>();
      for (unsigned i = 0; i < size; i++) {
        shareInfo si;
        serializeString(stream, si.type);
        si.count = stream.read<int64_t>();
        Info.push_back(si);
      }
    }
  }
  
  return !stream.eof();
}

void ShareStatsRecord::serializeKey(xmstream &stream) const
{
  stream.writebe<uint64_t>(Time);
}

void ShareStatsRecord::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);
  stream.write<uint64_t>(Time);
  stream.write<uint64_t>(Total);
  stream.write<uint32_t>(Info.size());
  for (auto I: Info) {
    serializeString(stream, I.type);
    stream.write<int64_t>(I.count);
  }
}

// ====================== payoutRecord ======================

bool PayoutDbRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) {
    deserializeString(stream, userId); 
    time = (time_t)stream.read<uint64_t>();
    value = stream.read<int64_t>();
    deserializeString(stream, transactionId);
  }
  
  return !stream.eof();  
}

void PayoutDbRecord::serializeKey(xmstream &stream) const
{
  serializeStringForKey(stream, userId);
  stream.writebe<uint64_t>(time);
}

void PayoutDbRecord::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);
  serializeString(stream, userId);
  stream.write<uint64_t>(time);
  stream.write<int64_t>(value);
  serializeString(stream, transactionId);
}
