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

// CPPSState serialization methods

void CPPSState::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, Time);
}

void CPPSState::serializeValue(xmstream &stream) const
{
  DbIo<CPPSState>::serialize(stream, *this);
}

bool CPPSState::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  DbIo<CPPSState>::unserialize(stream, *this);
  return !stream.eof();
}

// CBackendSettings::PPS methods

bool CBackendSettings::PPS::parseSaturationFunction(const std::string &name, ESaturationFunction *out)
{
  struct { const char *name; ESaturationFunction value; } table[] = {
    {"none",     ESaturationFunction::None},
    {"tanh",     ESaturationFunction::Tangent},
    {"clamp",    ESaturationFunction::Clamp},
    {"cubic",    ESaturationFunction::Cubic},
    {"softsign", ESaturationFunction::Softsign},
    {"norm",     ESaturationFunction::Norm},
    {"atan",     ESaturationFunction::Atan},
    {"exp",      ESaturationFunction::Exp},
  };

  if (name.empty()) {
    *out = ESaturationFunction::None;
    return true;
  }

  for (const auto &entry : table) {
    if (name == entry.name) {
      *out = entry.value;
      return true;
    }
  }

  return false;
}

const char *CBackendSettings::PPS::saturationFunctionName(ESaturationFunction value)
{
  switch (value) {
    case ESaturationFunction::None:     return "none";
    case ESaturationFunction::Tangent:  return "tanh";
    case ESaturationFunction::Clamp:    return "clamp";
    case ESaturationFunction::Cubic:    return "cubic";
    case ESaturationFunction::Softsign: return "softsign";
    case ESaturationFunction::Norm:     return "norm";
    case ESaturationFunction::Atan:     return "atan";
    case ESaturationFunction::Exp:      return "exp";
    default:                            return "unknown";
  }
}

const std::vector<const char*> &CBackendSettings::PPS::saturationFunctionNames()
{
  static const std::vector<const char*> names = {
    "none", "tanh", "clamp", "cubic", "softsign", "norm", "atan", "exp"
  };
  return names;
}

double CBackendSettings::PPS::saturateCoeff(double balanceInBlocks) const
{
  if (SaturationFunction == ESaturationFunction::None)
    return 1.0;

  double x = (SaturationB0 > 0.0) ? balanceInBlocks / SaturationB0 : 0.0;
  double s;
  switch (SaturationFunction) {
    case ESaturationFunction::Tangent:
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

  double a = (x >= 0.0) ? SaturationAPositive : SaturationANegative;
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
