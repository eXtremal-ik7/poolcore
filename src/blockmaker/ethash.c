#include "blockmaker/ethash.h"
#include "blockmaker/tiny_sha3.h"
#include "libp2pconfig.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
static inline uint32_t xswapu32(uint32_t value) { return __builtin_bswap32(value); }
#elif defined(_MSC_VER)
static inline int32_t xswapu32(int32_t value) { return _byteswap_ulong(value); }
#endif

#if defined(__GNUC__) || defined(__clang__)
__thread uint8_t CachedSeed[32];
__thread unsigned CachedEpochNumber;
#elif defined(_MSC_VER)
__declspec(thread) uint8_t CachedSeed[32];
__declspec(thread) unsigned CachedEpochNumber;
#endif

#if (IS_BIGENDIAN == 1)
static inline uint32_t xletohu32(uint32_t x) { return xswapu32(x); }
#else
static inline uint32_t xletohu32(uint32_t x) { return x; }
#endif

typedef struct ItemState {
  uint32_t *Cache;
  int64_t CacheItemsNum;
  uint32_t Seed;
  uint32_t Mix[16];
} ItemState;

static const uint32_t fnv_prime = 0x01000193;

static inline uint32_t fnv1(uint32_t u, uint32_t v)
{
  return (u * fnv_prime) ^ v;
}

static inline int isOddPrime(int number)
{
  int d;

  // Check factors up to sqrt(number).
  // To avoid computing sqrt, compare d*d <= number with 64-bit precision.
  for (d = 3; (int64_t)d * (int64_t)d <= (int64_t)number; d += 2) {
    if (number % d == 0)
      return 0;
  }

  return 1;
}

static inline int findLargestPrime(int upper_bound)
{
  int n = upper_bound;

  if (n < 2)
    return 0;

  if (n == 2)
    return 2;

  // If even number, skip it.
  if (n % 2 == 0)
    --n;

  // Test descending odd numbers.
  while (!isOddPrime(n))
    n -= 2;

  return n;
}

static inline int calculateLightCacheNumItems(int epoch_number)
{
  const int light_cache_init_size = 1 << 24;
  const int light_cache_growth = 1 << 17;
  const int item_size = 512/8;
  const int num_items_init = light_cache_init_size / item_size;
  const int num_items_growth = light_cache_growth / item_size;
  int num_items_upper_bound = num_items_init + epoch_number * num_items_growth;
  int num_items = findLargestPrime(num_items_upper_bound);
  return num_items;
}

static inline int calculateFullDatasetNumItems(int epoch_number)
{
  const int full_dataset_init_size = 1 << 30;
  const int full_dataset_growth = 1 << 23;
  const int item_size = 1024/8;
  const int num_items_init = full_dataset_init_size / item_size;
  const int num_items_growth = full_dataset_growth / item_size;
  int num_items_upper_bound = num_items_init + epoch_number * num_items_growth;
  int num_items = findLargestPrime(num_items_upper_bound);
  return num_items;
}

static inline size_t getLightCacheSize(int num_items)
{
  return (size_t)(num_items * 64);
}

static void calculateEpochSeed(uint32_t out[8], int epoch_number)
{
  memset(out, 0, 32);
  sha3_ctx_t ctx;
  for (int i = 0; i < epoch_number; i++) {
    sha3_init(&ctx, 32);
    sha3_update(&ctx, out, 32);
    sha3_final(out, &ctx, 1);
  }
}

static void buildLightCache(uint32_t *cache, unsigned num_items, const uint32_t seed[8])
{
  const int light_cache_rounds = 3;
  const unsigned item32_size = 16;

  {
    sha3_ctx_t ctx;
    sha3_init(&ctx, 64);
    sha3_update(&ctx, seed, 32);
    sha3_final(&cache[item32_size*0], &ctx, 1);
  }

  for (unsigned i = 1; i < num_items; ++i) {
    sha3_ctx_t ctx;
    sha3_init(&ctx, 64);
    sha3_update(&ctx, &cache[item32_size*(i-1)], 64);
    sha3_final(&cache[item32_size*i], &ctx, 1);
  }

  for (int q = 0; q < light_cache_rounds; ++q) {
    for (unsigned i = 0; i < num_items; ++i) {
      const uint32_t index_limit = (uint32_t)num_items;

      // Fist index: 4 first bytes of the item as little-endian integer.
      const uint32_t t = xletohu32(cache[item32_size*i]);
      const uint32_t v = t % index_limit;

      // Second index.
      const uint32_t w = (num_items + (i - 1)) % index_limit;

      uint64_t x[8];
      const uint64_t *pv = (const uint64_t*)(&cache[item32_size*v]);
      const uint64_t *pw = (const uint64_t*)(&cache[item32_size*w]);
      for (unsigned i = 0; i < 8; i++)
        x[i] = pv[i] ^ pw[i];

      {
        sha3_ctx_t ctx;
        sha3_init(&ctx, 64);
        sha3_update(&ctx, x, 64);
        sha3_final(&cache[item32_size*i], &ctx, 1);
      }
    }
  }
}

static void itemInit(ItemState *item, const EthashDag *context, int64_t index)
{
  const int size32 = 16;

  item->Cache = context->LightCache;
  item->CacheItemsNum = context->LightCacheItemsNum;
  item->Seed = (uint32_t)index;

  memcpy(item->Mix, &item->Cache[size32*(index % item->CacheItemsNum)], 64);
  item->Mix[0] ^= item->Seed;

  sha3_ctx_t ctx;
  sha3_init(&ctx, 64);
  sha3_update(&ctx, item->Mix, 64);
  sha3_final(item->Mix, &ctx, 1);
}

static void itemUpdate(ItemState *item, uint32_t round)
{
  const unsigned size32 = 16;

  const uint32_t t = fnv1(item->Seed ^ round, item->Mix[round % size32]);
  const uint64_t parent_index = t % item->CacheItemsNum;
  for (size_t i = 0; i < size32; i++)
    item->Mix[i] = fnv1(item->Mix[i], item->Cache[size32*parent_index + i]);
}

static void itemFinal(void *out, ItemState *item)
{
  sha3_ctx_t ctx;
  sha3_init(&ctx, 64);
  sha3_update(&ctx, item->Mix, 64);
  sha3_final(out, &ctx, 1);
}

int ethashGetEpochNumber(void *seed)
{
  if (memcmp(seed, CachedSeed, 32) == 0)
    return CachedEpochNumber;

  uint8_t localSeed[32];
  memset(localSeed, 0, 32);
  for (unsigned i = 0; i < 65536; i++) {
    if (memcmp(seed, localSeed, 32) == 0) {
      memcpy(CachedSeed, localSeed, 32);
      CachedEpochNumber = i;
      return i;
    }

    sha3_ctx_t ctx;
    sha3_init(&ctx, 32);
    sha3_update(&ctx, localSeed, 32);
    sha3_final(localSeed, &ctx, 1);
  }

  return -1;
}

EthashDag *ethashCreateDag(int epochNumber, int bigEpoch)
{
  const size_t context_alloc_size = 512/8;
  const int light_cache_num_items = calculateLightCacheNumItems(epochNumber);
  const int full_dataset_num_items = calculateFullDatasetNumItems(epochNumber);
  const size_t light_cache_size = getLightCacheSize(light_cache_num_items);
  const size_t alloc_size = context_alloc_size + light_cache_size;

  uint8_t *alloc_data = (uint8_t*)calloc(1, alloc_size);
  if (!alloc_data)
    return 0;

  uint32_t *light_cache = (uint32_t*)(alloc_data + context_alloc_size);

  uint32_t epochSeed[8];
  calculateEpochSeed(epochSeed, !bigEpoch ? epochNumber : epochNumber*2);
  buildLightCache(light_cache, light_cache_num_items, epochSeed);

  EthashDag *dag = (EthashDag*)alloc_data;
  dag->EpochNumber = epochNumber;
  dag->LightCacheItemsNum = light_cache_num_items;
  dag->LightCache = light_cache;
  dag->FullDatasetItemsNum = full_dataset_num_items;
  return dag;
}

void ethashCalculate(void *finalHash, void *mixHash, const void *headerHash, uint64_t nonce, const EthashDag *context)
{
  const int num_dataset_accesses = 64;

  // calculate seed
  uint8_t seedX[64];
  {
    sha3_ctx_t ctx;
    sha3_init(&ctx, 64);
    sha3_update(&ctx, headerHash, 32);
    sha3_update(&ctx, &nonce, 8);
    sha3_final(seedX, &ctx, 1);
  }

  // calculate mix hash
  const size_t num_words = 1024/32;
  const uint32_t index_limit = context->FullDatasetItemsNum;

  uint32_t mix[32];
  memcpy(mix, seedX, 64);
  memcpy(mix + 16, seedX, 64);
  const uint32_t seed_init = mix[0];


  for (uint32_t i = 0; i < num_dataset_accesses; ++i) {
    uint32_t newData32[32];
    const uint32_t p = fnv1(i ^ seed_init, mix[i % num_words]) % index_limit;

    {
      const int full_dataset_item_parents = 256;
      ItemState item0;
      ItemState item1;

      itemInit(&item0, context, p*2);
      itemInit(&item1, context, p*2+1);

      for (uint32_t j = 0; j < full_dataset_item_parents; ++j) {
        itemUpdate(&item0, j);
        itemUpdate(&item1, j);
      }

      itemFinal(newData32, &item0);
      itemFinal(newData32+16, &item1);
    }

    for (size_t j = 0; j < num_words; ++j)
      mix[j] = fnv1(mix[j], newData32[j]);
  }

  uint32_t *mixHash32 = (uint32_t*)mixHash;
  for (size_t i = 0; i < num_words; i += 4)
  {
      const uint32_t h1 = fnv1(mix[i], mix[i + 1]);
      const uint32_t h2 = fnv1(h1, mix[i + 2]);
      const uint32_t h3 = fnv1(h2, mix[i + 3]);
      mixHash32[i / 4] = h3;
  }

  {
    sha3_ctx_t ctx;
    sha3_init(&ctx, 32);
    sha3_update(&ctx, seedX, 64);
    sha3_update(&ctx, mixHash32, 32);
    sha3_final(finalHash, &ctx, 1);
  }
}
