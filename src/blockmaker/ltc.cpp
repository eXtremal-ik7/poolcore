#include "poolcommon/arith_uint256.h"
#include "blockmaker/ltc.h"
#include "blockmaker/scrypt.h"

static inline double getDifficulty(uint32_t bits)
{
  int nShift = (bits >> 24) & 0xff;
  double dDiff =
      (double)0x0000ffff / (double)(bits & 0x00ffffff);

  while (nShift < 29)
  {
      dDiff *= 256.0;
      nShift++;
  }
  while (nShift > 29)
  {
      dDiff /= 256.0;
      nShift--;
  }

  return dDiff;
}

bool LTC::Proto::checkConsensus(const LTC::Proto::BlockHeader &header, CheckConsensusCtx&, LTC::Proto::ChainParams &chainParams, double *shareDiff)
{
  arith_uint256 scryptHash;
  scrypt_1024_1_1_256(reinterpret_cast<const char*>(&header), reinterpret_cast<char*>(scryptHash.begin()));
  *shareDiff = getDifficulty(scryptHash.GetCompact());

  bool fNegative;
  bool fOverflow;
  arith_uint256 bnTarget;

  bnTarget.SetCompact(header.nBits, &fNegative, &fOverflow);

  // Check range
  if (fNegative || bnTarget == 0 || fOverflow)
      return false;

  // Check proof of work matches claimed amount
  if (scryptHash > bnTarget)
      return false;

  return true;
}
