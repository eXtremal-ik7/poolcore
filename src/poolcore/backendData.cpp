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

static inline void deserializeString(xmstream &stream, std::string &S)
{
  size_t size = stream.read<uint32_t>();
  const char *data = stream.seek<const char>(size);
  if (data)
    S.assign(data, size);
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
    deserializeString(stream, userId);
    payoutValue = stream.read<int64_t>();
    queued = stream.read<int64_t>();
    deserializeString(stream, asyncOpId);
  }
  
  return !stream.eof();
}

void payoutElement::serializeValue(xmstream &stream) const
{
  stream.write(CurrentRecordVersion);
  serializeString(stream, userId);
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
    serializeString(stream, p.userId);
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
        deserializeString(stream, element.userId);
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
    fprintf(stderr, " * userId: %s\n", p.userId.c_str());
    fprintf(stderr, " * payoutValue: %" PRId64 "\n", p.payoutValue);
    fprintf(stderr, " * queued: %" PRId64 "\n", p.queued);    
  }  
}


bool userBalance::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) { 
    deserializeString(stream, userId);
    deserializeString(stream, name);
    deserializeString(stream, email);
    deserializeString(stream, passwordHash);
    balance = stream.read<int64_t>();
    requested = stream.read<int64_t>();
    paid = stream.read<int64_t>();
    minimalPayout = stream.read<int64_t>();
  }
  
  return !stream.eof();
}

void userBalance::serializeKey(xmstream &stream) const
{
  serializeStringForKey(stream, userId);
}

void userBalance::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);  
  serializeString(stream, userId);
  serializeString(stream, name);
  serializeString(stream, email);
  serializeString(stream, passwordHash);
  stream.write<int64_t>(balance);
  stream.write<int64_t>(requested);
  stream.write<int64_t>(paid);
  stream.write<int64_t>(minimalPayout);
}

// ====================== foundBlock ====================== \\

bool foundBlock::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) {
    height = stream.read<uint32_t>();
    deserializeString(stream, hash);
    time = (time_t)stream.read<uint64_t>();
    availableCoins = stream.read<int64_t>();
    deserializeString(stream, foundBy);
  }
  
  return !stream.eof();
}


void foundBlock::serializeKey(xmstream &stream) const
{
  stream.writebe<uint32_t>(height);
  serializeStringForKey(stream, hash);
}

void foundBlock::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);
  stream.write<uint32_t>(height);
  serializeString(stream, hash);
  stream.write<uint64_t>(time);
  stream.write<int64_t>(availableCoins);
  serializeString(stream, foundBy);
}

// ====================== poolBalance ====================== \\

bool poolBalance::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) {
    time = (time_t)stream.read<uint64_t>();
    balance = stream.read<int64_t>();
    immature = stream.read<int64_t>();
    users = stream.read<int64_t>();
    queued = stream.read<int64_t>();
    net = stream.read<int64_t>();
  }
  
  return !stream.eof();
}

void poolBalance::serializeKey(xmstream &stream) const
{
  stream.writebe<uint64_t>(time);
}

void poolBalance::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);
  stream.write<uint64_t>(time);
  stream.write<int64_t>(balance);
  stream.write<int64_t>(immature);
  stream.write<int64_t>(users);
  stream.write<int64_t>(queued);
  stream.write<int64_t>(net);  
}

// ====================== siteStats ====================== \\

bool siteStats::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) {
    deserializeString(stream, userId);
    time = (time_t)stream.read<uint64_t>();
    clients = stream.read<uint32_t>();
    workers = stream.read<uint32_t>();
    cpus = stream.read<uint32_t>();
    gpus = stream.read<uint32_t>();
    asics = stream.read<uint32_t>();
    other = stream.read<uint32_t>();
    latency = stream.read<uint32_t>();
    power = stream.read<uint64_t>();
  }
  
  return !stream.eof();  
}

void siteStats::serializeKey(xmstream &stream) const
{
  serializeStringForKey(stream, userId);
  stream.writebe<uint64_t>(time);
}

void siteStats::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);
  serializeString(stream, userId);
  stream.write<uint64_t>(time);
  stream.write<uint32_t>(clients);
  stream.write<uint32_t>(workers);
  stream.write<uint32_t>(cpus);
  stream.write<uint32_t>(gpus);
  stream.write<uint32_t>(asics);  
  stream.write<uint32_t>(other);
  stream.write<uint32_t>(latency);
  stream.write<uint64_t>(power);
}

// ====================== clientStats ====================== \\

bool clientStats::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) {
    deserializeString(stream, userId);
    deserializeString(stream, workerId);
    time = (time_t)stream.read<uint64_t>();
    power = stream.read<uint64_t>();
    latency = stream.read<int32_t>();
    deserializeString(stream, address);
    unitType = stream.read<int32_t>();
    units = stream.read<uint32_t>();
    temp = stream.read<uint32_t>();
  }
  
  return !stream.eof();    
}

void clientStats::serializeKey(xmstream &stream) const
{
  serializeStringForKey(stream, userId);
  serializeStringForKey(stream, workerId);
  stream.writebe<uint64_t>(time);
}

void clientStats::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);
  serializeString(stream, userId);
  serializeString(stream, workerId);
  stream.write<uint64_t>(time);
  stream.write<uint64_t>(power);
  stream.write<int32_t>(latency);
  serializeString(stream, address);
  stream.write<int32_t>(unitType);
  stream.write<uint32_t>(units);
  stream.write<uint32_t>(temp);
}

// ====================== shareStats ====================== \\

bool shareStats::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  uint32_t version = stream.read<uint32_t>();
  if (version >= 1) {
    time = (time_t)stream.read<uint64_t>();
    total = stream.read<int64_t>();
    
    {
      info.clear();
      unsigned size = stream.read<uint32_t>();
      for (unsigned i = 0; i < size; i++) {
        shareInfo si;
        serializeString(stream, si.type);
        si.count = stream.read<int64_t>();
        info.push_back(si);
      }
    }
  }
  
  return !stream.eof();
}

void shareStats::serializeKey(xmstream &stream) const
{
  stream.writebe<uint64_t>(time);
}

void shareStats::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);
  stream.write<uint64_t>(time);
  stream.write<uint64_t>(total);
  stream.write<uint32_t>(info.size());
  for (auto I: info) {
    serializeString(stream, I.type);
    stream.write<int64_t>(I.count);
  }
}

// ====================== payoutRecord ====================== \\

bool payoutRecord::deserializeValue(const void *data, size_t size)
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

void payoutRecord::serializeKey(xmstream &stream) const
{
  serializeStringForKey(stream, userId);
  stream.writebe<uint64_t>(time);
}

void payoutRecord::serializeValue(xmstream &stream) const
{
  stream.write<uint32_t>(CurrentRecordVersion);
  serializeString(stream, userId);
  stream.write<uint64_t>(time);
  stream.write<int64_t>(value);
  serializeString(stream, transactionId);
}
