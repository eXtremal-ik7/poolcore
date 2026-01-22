#include "blockmaker/dash.h"
#include "blockmaker/x11.h"
#include "poolcommon/uint.h"

CCheckStatus DASH::Proto::checkPow(const BlockHeader &header, uint32_t nBits) {
    CCheckStatus status;
    // Compute X11 hash
    UInt<256> x11Hash;
    x11_hash(reinterpret_cast<const uint8_t*>(&header), sizeof(header), x11Hash.rawData());
    for (unsigned i = 0; i < 4; i++)
      x11Hash.data()[i] = readle(x11Hash.data()[i]);

    // Calculate share difficulty from bits
    status.ShareDiff = BTC::difficultyFromBits(uint256GetCompact(x11Hash), 29);

    // Build target from compact
    bool fNegative = false;
    bool fOverflow = false;
    UInt<256> bnTarget = uint256Compact(nBits, &fNegative, &fOverflow);

    // Range check
    if (fNegative || bnTarget == 0 || fOverflow)
        return status;

    // Proof-of-work check
    if (x11Hash > bnTarget)
        return status;

    status.IsBlock = true;
    return status;
}
