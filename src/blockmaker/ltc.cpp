#include "poolcommon/uint.h"
#include "blockmaker/ltc.h"
#include "blockmaker/scrypt.h"

static inline UInt<256> powValue(const LTC::Proto::BlockHeader &header)
{
  UInt<256> value;
  scrypt_1024_1_1_256(reinterpret_cast<const char*>(&header), reinterpret_cast<uint8_t*>(value.data()));
  for (unsigned i = 0; i < 4; i++)
    value.data()[i] = readle(value.data()[i]);
  return value;
}

CCheckStatus LTC::Proto::checkPow(const Proto::BlockHeader &header, uint32_t nBits)
{
  CCheckStatus status;
  UInt<256> scryptHash = powValue(header);
  status.ShareDiff = BTC::difficultyFromBits(uint256GetCompact(scryptHash), 29);

  bool fNegative;
  bool fOverflow;
  UInt<256> bnTarget = uint256Compact(nBits, &fNegative, &fOverflow);

  // Check range
  if (fNegative || bnTarget == 0 || fOverflow)
    return status;

  // Check proof of work matches claimed amount
  if (scryptHash > bnTarget)
    return status;

  status.IsBlock = true;
  return status;
}
