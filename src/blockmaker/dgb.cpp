#include "blockmaker/dgb.h"

namespace DGB {

bool Proto::checkConsensus(const DGB::Proto::BlockHeader &header, CheckConsensusCtx&, DGB::Proto::ChainParams &chainParams, double *shareDiff)
{
  // TODO: implement
  *shareDiff = 0;
  return false;
}

}
