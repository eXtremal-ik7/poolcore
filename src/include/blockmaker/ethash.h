#pragma once

#include <stdint.h>

typedef struct EthashDag {
  int EpochNumber;
  int LightCacheItemsNum;
  int FullDatasetItemsNum;
  uint32_t *LightCache;
} EthashDag;

int ethashGetEpochNumber(void *seed);
EthashDag *ethashCreateDag(int epochNumber, int bigEpoch);
void ethashCalculate(void *finalHash, void *mixHash, const void *headerHash, uint64_t nonce, const EthashDag *context);
