#include "poolcommon/pool_generated.h"
#include "main.h"

extern CBlockIndex* pindexBestHeader;

const char *getCoinName() { return "BTC"; }

void *getCurrentBlock()
{
  return pindexBestHeader;
}

void createBlockRecord(void *block, BlockT &out)
{
  if (block) {
    CBlockIndex *blockIndex = (CBlockIndex*)block;
    out.height = blockIndex->nHeight;
    out.bits = blockIndex->nBits;
    out.hash = blockIndex->phashBlock->GetHex();
    out.prevhash = blockIndex->pprev->phashBlock->GetHex();    
  } else {
    out.height = 0;
    out.bits = 0;
    out.hash = "";
    out.prevhash = "";    
  }
}
