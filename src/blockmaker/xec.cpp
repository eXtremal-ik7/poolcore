#include "blockmaker/xec.h"
#include "poolcommon/arith_uint256.h"
#include <math.h>

namespace XEC {

static arith_uint256 computeNextTarget(int64_t now,
                                       uint32_t prevBits,
                                       const int64_t prevHeaderTime[4],
                                       uint32_t headerBits)
{
  int64_t one = 1;
  arith_uint256 prev256;
  arith_uint256 header256;
  prev256.SetCompact(prevBits);
  header256.SetCompact(headerBits);
  double prevTarget = prev256.getdouble();
  double headerTarget = header256.getdouble();

  int64_t diffTime0 = std::max(one, now - prevHeaderTime[0]);
  double target0 = prevTarget * 4.9192018423e-14 * pow(diffTime0, 5);

  int64_t diffTime1 = std::max(one, now - prevHeaderTime[1]);
  double target1 = prevTarget * 4.8039080491e-17 * pow(diffTime1, 5);

  int64_t diffTime2 = std::max(one, now - prevHeaderTime[2]);
  double target2 = prevTarget * 4.9192018423e-19 * pow(diffTime2, 5);

  int64_t diffTime3 = std::max(one, now - prevHeaderTime[3]);
  double target3 = prevTarget * 4.6913164542e-20 * pow(diffTime3, 5);

  double nextTarget = std::min({target0, target1, target2, target3});

  // The real time target is never higher (less difficult) than the normal
  // target.
  if (nextTarget < headerTarget)
    return arith_uint256(nextTarget);
  else
    return header256;
}

void Proto::CheckConsensusCtx::initialize(CBlockTemplate &blockTemplate, const std::string&)
{
  if (!blockTemplate.Document.HasMember("result") || !blockTemplate.Document["result"].IsObject())
    return;

  rapidjson::Value &resultValue = blockTemplate.Document["result"];
  if (!resultValue.HasMember("rtt") || !resultValue["rtt"].IsObject())
    return;
  rapidjson::Value &rttValue = resultValue["rtt"];

  if (!rttValue.HasMember("prevheadertime") || !rttValue["prevheadertime"].IsArray() ||
      !rttValue.HasMember("prevbits") || !rttValue["prevbits"].IsString() || rttValue["prevbits"].GetStringLength() != 8)
    return;

  auto prevHeaderTimeValue = rttValue["prevheadertime"].GetArray();
  if (prevHeaderTimeValue.Size() != 4)
    return;
  for (unsigned i = 0; i < 4; i++) {
    if (!prevHeaderTimeValue[i].IsInt64())
      return;
    PrevHeaderTime[i] = prevHeaderTimeValue[i].GetInt64();
  }

  auto prevBitsValue = rttValue["prevbits"].GetString();
  PrevBits = strtoul(prevBitsValue, nullptr, 16);
  HasRtt = true;
  {
    rapidjson::Value &bitsValue = resultValue["bits"];
    uint64_t bits = strtoul(bitsValue.GetString(), nullptr, 16);

    int64_t now = time(nullptr);
    LOG_F(WARNING, "bits: %08x (%.2lf) now: %lli prevBits: %08x prevheadertime: %lli %lli %lli %lli", bits, BTC::difficultyFromBits(bits, 29), now, PrevBits, PrevHeaderTime[0], PrevHeaderTime[1], PrevHeaderTime[2], PrevHeaderTime[3]);
    for (int64_t n: {now, now+1, now+5, now+10, now+20, now+30, now+60, now+120, now+240, now+300, now+480, now+600}) {
      arith_uint256 t = computeNextTarget(n, PrevBits, PrevHeaderTime, bits);
      LOG_F(WARNING, "target for %lli (+%lli): %08x (%.2lf)", n, n - now, t.GetCompact(), BTC::difficultyFromBits(t.GetCompact(), 29));
    }
  }
}


CCheckStatus Proto::checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams&)
{
  CCheckStatus status;
  bool fNegative;
  bool fOverflow;
  arith_uint256 defaultTarget;
  arith_uint256 hash = UintToArith256(header.GetHash());
  defaultTarget.SetCompact(header.nBits, &fNegative, &fOverflow);
  status.ShareDiff = BTC::difficultyFromBits(hash.GetCompact(), 29);

  // Check range
  if (fNegative || defaultTarget == 0 || fOverflow)
    return status;

  if (ctx.HasRtt) {
    arith_uint256 adjustedTarget = computeNextTarget(time(nullptr), ctx.PrevBits, ctx.PrevHeaderTime, header.nBits);
    status.IsBlock = hash <= adjustedTarget;
    status.IsPendingBlock = hash <= defaultTarget;
    // LOG_F(WARNING, "rtt: %08x (%.2lf) header: %08x (%.2lf)", adjustedTarget.GetCompact(), BTC::difficultyFromBits(adjustedTarget.GetCompact(), 29), header.nBits, BTC::difficultyFromBits(header.nBits, 29));
  } else {
    status.IsBlock = hash <= defaultTarget;
    status.IsPendingBlock = false;
  }

  // Check proof of work matches claimed amount
  return status;
}

double Proto::expectedWork(const XEC::Proto::BlockHeader &header, const CheckConsensusCtx &ctx)
{
  if (ctx.HasRtt) {
    arith_uint256 adjustedTarget = computeNextTarget(time(nullptr), ctx.PrevBits, ctx.PrevHeaderTime, header.nBits);
    return BTC::difficultyFromBits(adjustedTarget.GetCompact(), 29);
  } else {
    return BTC::difficultyFromBits(header.nBits, 29);
  }
}

}
