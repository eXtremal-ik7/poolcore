#pragma once

#include <stdint.h>

typedef struct EthashDag {
  int EpochNumber;
  int LightCacheItemsNum;
  int FullDatasetItemsNum;
  uint32_t *LightCache;
} EthashDag;

EthashDag *ethashCreateDag(int epoch_number);
void ethashCalculate(void *finalHash, void *mixHash, const void *headerHash, uint64_t nonce, const EthashDag *context);
