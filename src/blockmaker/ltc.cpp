#include "poolcommon/arith_uint256.h"
#include "blockmaker/ltc.h"
#include "blockmaker/scrypt.h"

bool LTC::Proto::checkConsensus(const LTC::Proto::BlockHeader &header, CheckConsensusCtx&, LTC::Proto::ChainParams &chainParams)
{
  uint256 scryptHash;
  scrypt_1024_1_1_256(reinterpret_cast<const char*>(&header), reinterpret_cast<char*>(scryptHash.begin()));

  bool fNegative;
  bool fOverflow;
  arith_uint256 bnTarget;

  bnTarget.SetCompact(header.nBits, &fNegative, &fOverflow);

  // Check range
  if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(chainParams.powLimit))
      return false;

  // Check proof of work matches claimed amount
  if (UintToArith256(scryptHash) > bnTarget)
      return false;

  return true;
}
