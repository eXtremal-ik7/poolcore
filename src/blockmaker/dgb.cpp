#include "blockmaker/dgb.h"

namespace DGB {

bool Proto::checkConsensus(const DGB::Proto::BlockHeader&, CheckConsensusCtx&, DGB::Proto::ChainParams&, double *shareDiff)
{
  // TODO: implement
  *shareDiff = 0;
  return false;
}

}
