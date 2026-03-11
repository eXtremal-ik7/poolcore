#include "poolcore/accountingData.h"
#include <math.h>

// DbIo<CPPSBalanceSnapshot>

void DbIo<CPPSBalanceSnapshot>::serialize(xmstream &dst, const CPPSBalanceSnapshot &data)
{
  DbIo<UInt<384>>::serialize(dst, data.Balance);
  DbIo<double>::serialize(dst, data.TotalBlocksFound);
  DbIo<Timestamp>::serialize(dst, data.Time);
}

void DbIo<CPPSBalanceSnapshot>::unserialize(xmstream &src, CPPSBalanceSnapshot &data)
{
  DbIo<UInt<384>>::unserialize(src, data.Balance);
  DbIo<double>::unserialize(src, data.TotalBlocksFound);
  DbIo<Timestamp>::unserialize(src, data.Time);
}

// DbIo<CPPSState>

void DbIo<CPPSState>::serialize(xmstream &dst, const CPPSState &data)
{
  DbIo<UInt<384>>::serialize(dst, data.Balance);
  DbIo<UInt<384>>::serialize(dst, data.ReferenceBalance);
  DbIo<UInt<384>>::serialize(dst, data.LastBaseBlockReward);
  DbIo<double>::serialize(dst, data.TotalBlocksFound);
  DbIo<double>::serialize(dst, data.OrphanBlocks);
  DbIo<CPPSBalanceSnapshot>::serialize(dst, data.Min);
  DbIo<CPPSBalanceSnapshot>::serialize(dst, data.Max);
  DbIo<double>::serialize(dst, data.LastSaturateCoeff);
  DbIo<UInt<384>>::serialize(dst, data.LastAverageTxFee);
  DbIo<Timestamp>::serialize(dst, data.Time);
}

void DbIo<CPPSState>::unserialize(xmstream &src, CPPSState &data)
{
  DbIo<UInt<384>>::unserialize(src, data.Balance);
  DbIo<UInt<384>>::unserialize(src, data.ReferenceBalance);
  DbIo<UInt<384>>::unserialize(src, data.LastBaseBlockReward);
  DbIo<double>::unserialize(src, data.TotalBlocksFound);
  DbIo<double>::unserialize(src, data.OrphanBlocks);
  DbIo<CPPSBalanceSnapshot>::unserialize(src, data.Min);
  DbIo<CPPSBalanceSnapshot>::unserialize(src, data.Max);
  DbIo<double>::unserialize(src, data.LastSaturateCoeff);
  DbIo<UInt<384>>::unserialize(src, data.LastAverageTxFee);
  DbIo<Timestamp>::unserialize(src, data.Time);
}

// DbIo<CRoundBestShareData>

void DbIo<CRoundBestShareData>::serialize(xmstream &dst, const CRoundBestShareData &data)
{
  DbIo<uint32_t>::serialize(dst, 1);
  DbIo<std::optional<UInt<256>>>::serialize(dst, data.Hash);
  DbIo<double>::serialize(dst, data.ShareDifficulty);
  DbIo<double>::serialize(dst, data.BlockDifficulty);
  DbIo<UInt<256>>::serialize(dst, data.ExpectedWork);
  DbIo<Timestamp>::serialize(dst, data.Time);
}

void DbIo<CRoundBestShareData>::unserialize(xmstream &src, CRoundBestShareData &data)
{
  uint32_t version;
  DbIo<uint32_t>::unserialize(src, version);
  if (version == 1) {
    DbIo<std::optional<UInt<256>>>::unserialize(src, data.Hash);
    DbIo<double>::unserialize(src, data.ShareDifficulty);
    DbIo<double>::unserialize(src, data.BlockDifficulty);
    DbIo<UInt<256>>::unserialize(src, data.ExpectedWork);
    DbIo<Timestamp>::unserialize(src, data.Time);
  }
}

// CPPSState serialization methods

std::string CPPSState::getPartitionId() const { return partByTime(Time.toUnixTime()); }

void CPPSState::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, Time);
}

void CPPSState::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  DbIo<CPPSState>::serialize(stream, *this);
}

bool CPPSState::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version == 1) {
    DbIo<CPPSState>::unserialize(stream, *this);
  }
  return !stream.eof();
}

// saturateCoeff — free function (was CBackendSettings::PPS::saturateCoeff)

double saturateCoeff(const CBackendPPS &pps, double balanceInBlocks)
{
  if (pps.SaturationFunction == ESaturationFunction::None)
    return 1.0;

  double x = (pps.SaturationB0 > 0.0) ? balanceInBlocks / pps.SaturationB0 : 0.0;
  double s;
  switch (pps.SaturationFunction) {
    case ESaturationFunction::Tanh:
      s = std::tanh(x);
      break;
    case ESaturationFunction::Clamp:
      s = std::max(-1.0, std::min(1.0, x));
      break;
    case ESaturationFunction::Cubic:
      if (x <= -1.0) s = -1.0;
      else if (x >= 1.0) s = 1.0;
      else s = (3.0 * x - x * x * x) * 0.5;
      break;
    case ESaturationFunction::Softsign:
      s = x / (1.0 + std::abs(x));
      break;
    case ESaturationFunction::Norm:
      s = x / std::sqrt(1.0 + x * x);
      break;
    case ESaturationFunction::Atan:
      s = (2.0 / M_PI) * std::atan(x);
      break;
    case ESaturationFunction::Exp: {
      double ax = std::abs(x);
      s = 1.0 - std::exp(-ax);
      if (x < 0.0) s = -s;
      break;
    }
    default:
      s = 0.0;
      break;
  }

  double a = (x >= 0.0) ? pps.SaturationAPositive : pps.SaturationANegative;
  return 1.0 + a * s;
}

// CPPSState methods

double CPPSState::balanceInBlocks(const UInt<384> &balance, const UInt<384> &baseBlockReward)
{
  if (baseBlockReward.isZero())
    return 0.0;
  bool neg = balance.isNegative();
  UInt<384> abs = balance;
  if (neg)
    abs.negate();
  double result = UInt<384>::fpdiv(abs, baseBlockReward);
  return neg ? -result : result;
}

double CPPSState::sqLambda(
  const UInt<384> &balance,
  const UInt<384> &baseBlockReward,
  double totalBlocksFound)
{
  if (totalBlocksFound <= 0.0)
    return 0.0;
  return balanceInBlocks(balance, baseBlockReward) / std::sqrt(totalBlocksFound);
}

static bool signedLess(const UInt<384> &a, const UInt<384> &b)
{
  bool aNeg = a.isNegative();
  bool bNeg = b.isNegative();
  if (aNeg != bNeg) return aNeg;
  return a < b;
}

void CPPSState::updateMinMax(Timestamp now)
{
  if (signedLess(ReferenceBalance, Min.Balance)) {
    Min.Balance = ReferenceBalance;
    Min.TotalBlocksFound = TotalBlocksFound;
    Min.Time = now;
  }
  if (signedLess(Max.Balance, ReferenceBalance)) {
    Max.Balance = ReferenceBalance;
    Max.TotalBlocksFound = TotalBlocksFound;
    Max.Time = now;
  }
}
