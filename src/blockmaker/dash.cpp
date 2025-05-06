#include "poolcommon/arith_uint256.h"
#include "blockmaker/dash.h"
#include "blockmaker/x11.h"

CCheckStatus DASH::Proto::checkPow(const BlockHeader &header, uint32_t nBits) {
    CCheckStatus status;
    // Compute X11 hash
    arith_uint256 x11Hash;
    x11_hash(reinterpret_cast<const uint8_t*>(&header), sizeof(header), x11Hash.begin());
    // Calculate share difficulty from bits
    status.ShareDiff = BTC::difficultyFromBits(x11Hash.GetCompact(), 29);

    // Build target from compact
    bool fNegative = false;
    bool fOverflow = false;
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Range check
    if (fNegative || bnTarget == 0 || fOverflow)
        return status;

    // Proof-of-work check
    if (x11Hash > bnTarget)
        return status;

    status.IsBlock = true;
    return status;
}