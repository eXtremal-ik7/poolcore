#include "struct2.h"
#include "poolcore/backendData.h"
#include <string.h>

// StatsRecord2

bool StatsRecord2::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, Login);
    dbIoUnserialize(stream, WorkerId);
    dbIoUnserialize(stream, Time);
    dbIoUnserialize(stream, ShareCount);
    dbIoUnserialize(stream, ShareWork);
    if (stream.remaining()) {
      dbIoUnserialize(stream, PrimePOWTarget);
      dbIoUnserialize(stream, PrimePOWShareCount);
    }
  }

  return !stream.eof();
}

// FoundBlockRecord2

bool FoundBlockRecord2::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
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

// CPPLNSPayout2

bool CPPLNSPayout2::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version == 1) {
    dbIoUnserialize(stream, Login);
    dbIoUnserialize(stream, RoundStartTime);
    dbIoUnserialize(stream, BlockHash);
    dbIoUnserialize(stream, BlockHeight);
    dbIoUnserialize(stream, RoundEndTime);
    dbIoUnserialize(stream, PayoutValue);
    dbIoUnserialize(stream, PayoutValueWithoutFee);
    dbIoUnserialize(stream, AcceptedWork);
    dbIoUnserialize(stream, PrimePOWTarget);
    dbIoUnserialize(stream, RateToBTC);
    dbIoUnserialize(stream, RateBTCToUSD);
  }

  return !stream.eof();
}

// PayoutDbRecord2

bool PayoutDbRecord2::deserializeValue(xmstream &stream)
{
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, UserId);
    dbIoUnserialize(stream, Time);
    dbIoUnserialize(stream, Value);
    dbIoUnserialize(stream, TransactionId);
    dbIoUnserialize(stream, TransactionData);
    dbIoUnserialize(stream, Status);
    if (version >= 2) {
      dbIoUnserialize(stream, TxFee);
    }
  }

  return !stream.eof();
}

bool PayoutDbRecord2::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  return deserializeValue(stream);
}

// PoolBalanceRecord2

bool PoolBalanceRecord2::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, Time);
    dbIoUnserialize(stream, BalanceWithFractional);
    dbIoUnserialize(stream, Immature);
    dbIoUnserialize(stream, Users);
    dbIoUnserialize(stream, Queued);
    dbIoUnserialize(stream, ConfirmationWait);
    dbIoUnserialize(stream, Net);
  }

  return !stream.eof();
}

// UserSettingsRecord2

bool UserSettingsRecord2::deserializeValue(const void *data, size_t size)
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

void UserSettingsRecord2::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, Login);
  dbKeyIoSerialize(stream, Coin);
}

void UserSettingsRecord2::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, Login);
  dbIoSerialize(stream, Coin);
  dbIoSerialize(stream, Address);
  dbIoSerialize(stream, MinimalPayout);
  dbIoSerialize(stream, AutoPayout);
}

// MiningRound2

bool MiningRound2::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);

  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version == 1) {
    std::vector<UserShareValue1> userShares1;
    std::vector<PayoutDbRecord2> payouts1;

    dbIoUnserialize(stream, Height);
    dbIoUnserialize(stream, BlockHash);
    dbIoUnserialize(stream, EndTime);

    StartTime = 0;

    dbIoUnserialize(stream, TotalShareValue);
    dbIoUnserialize(stream, AvailableCoins);

    dbIoUnserialize(stream, userShares1);
    {
      for (const auto &s: userShares1)
        UserShares.emplace_back(s.userId, s.shareValue, 0.0);
    }

    dbIoUnserialize(stream, payouts1);
    {
      for (const auto &p: payouts1)
        Payouts.emplace_back(p.UserId, p.Value, p.Value, 0.0);
    }

    if (stream.remaining()) {
      dbIoUnserialize(stream, FoundBy);
      dbIoUnserialize(stream, ExpectedWork);
      dbIoUnserialize(stream, AccumulatedWork);
      dbIoUnserialize(stream, TxFee);
    }

    PrimePOWTarget = -1U;
  } else if (version == 2) {
    dbIoUnserialize(stream, Height);
    dbIoUnserialize(stream, BlockHash);
    dbIoUnserialize(stream, EndTime);
    dbIoUnserialize(stream, StartTime);
    dbIoUnserialize(stream, TotalShareValue);
    dbIoUnserialize(stream, AvailableCoins);
    dbIoUnserialize(stream, UserShares);
    dbIoUnserialize(stream, Payouts);
    dbIoUnserialize(stream, FoundBy);
    dbIoUnserialize(stream, ExpectedWork);
    dbIoUnserialize(stream, AccumulatedWork);
    dbIoUnserialize(stream, TxFee);
    dbIoUnserialize(stream, PrimePOWTarget);
  }

  return !stream.eof();
}

// UserBalanceRecord2

bool UserBalanceRecord2::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, Login);
    dbIoUnserialize(stream, BalanceWithFractional);
    dbIoUnserialize(stream, Requested);
    dbIoUnserialize(stream, Paid);
  }

  return !stream.eof();
}

void UserBalanceRecord2::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, Login);
}

void UserBalanceRecord2::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, Login);
  dbIoSerialize(stream, BalanceWithFractional);
  dbIoSerialize(stream, Requested);
  dbIoSerialize(stream, Paid);
}

// ShareLogIo<CShareV1>

void ShareLogIo<CShareV1>::serialize(xmstream &out, const CShareV1 &data)
{
  out.writele<uint32_t>(data.CurrentRecordVersion);
  out.writele<uint64_t>(data.UniqueShareId);
  DbIo<std::string>::serialize(out, data.userId);
  DbIo<std::string>::serialize(out, data.workerId);
  out.write<double>(data.WorkValue);
  DbIo<int64_t>::serialize(out, data.Time);
  DbIo<uint32_t>::serialize(out, data.ChainLength);
  DbIo<uint32_t>::serialize(out, data.PrimePOWTarget);
}

void ShareLogIo<CShareV1>::unserialize(xmstream &out, CShareV1 &data)
{
  uint32_t version;
  version = out.readle<uint32_t>();
  data.UniqueShareId = out.readle<uint64_t>();
  DbIo<std::string>::unserialize(out, data.userId);
  DbIo<std::string>::unserialize(out, data.workerId);
  data.WorkValue = out.read<double>();
  DbIo<int64_t>::unserialize(out, data.Time);
  DbIo<uint32_t>::unserialize(out, data.ChainLength);
  DbIo<uint32_t>::unserialize(out, data.PrimePOWTarget);
}

// CCoinLibraryOld2

CCoinInfoOld2 CCoinLibraryOld2::get(const char *coinName)
{
  CCoinInfoOld2 info;

  // Equihash-based algorithms and coins use WorkMultiplier=256.0
  static const char *equihashNames[] = {
    "equihash.200.9", "equihash.184.7", "equihash.48.5",
    "ARRR", "ARRR.testnet", "ARRR.regtest",
    "KMD", "KMD.testnet", "KMD.regtest",
    "ZEC", "ZEC.testnet", "ZEC.regtest",
    "ZEN", "ZEN.testnet", "ZEN.regtest"
  };

  for (const char *name : equihashNames) {
    if (strcmp(coinName, name) == 0) {
      info.WorkMultiplier = 256.0;
      return info;
    }
  }

  return info;
}
