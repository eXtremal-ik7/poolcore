#include "poolcommon/arith_uint256.h"
#include "blockmaker/dash.h"
#include "blockmaker/x11.h"

CCheckStatus DASH::Proto::checkPow(const Proto::BlockHeader &header, uint32_t nBits)
{
  CCheckStatus status;
  arith_uint256 x11Hash;
  hashX11(reinterpret_cast<const char*>(&header), x11Hash.begin());
  status.ShareDiff = BTC::difficultyFromBits(x11Hash.GetCompact(), 29);

  bool fNegative;
  bool fOverflow;
  arith_uint256 bnTarget;

  bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

  if (fNegative || bnTarget == 0 || fOverflow)
      return status;

  if (x11Hash > bnTarget)
      return status;

  status.IsBlock = true;
  return status;
}
