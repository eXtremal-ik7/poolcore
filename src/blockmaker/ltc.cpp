#include "poolcommon/arith_uint256.h"
#include "blockmaker/ltc.h"
#include "blockmaker/scrypt.h"

CCheckStatus LTC::Proto::checkPow(const Proto::BlockHeader &header, uint32_t nBits)
{
  CCheckStatus status;
  arith_uint256 scryptHash;
  scrypt_1024_1_1_256(reinterpret_cast<const char*>(&header), reinterpret_cast<char*>(scryptHash.begin()));
  status.ShareDiff = BTC::difficultyFromBits(scryptHash.GetCompact(), 29);

  bool fNegative;
  bool fOverflow;
  arith_uint256 bnTarget;

  bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

  // Check range
  if (fNegative || bnTarget == 0 || fOverflow)
      return status;

  // Check proof of work matches claimed amount
  if (scryptHash > bnTarget)
      return status;

  status.IsBlock = true;
  return status;
}
