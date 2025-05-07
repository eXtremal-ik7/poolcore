#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Low level
void sha3llRound800(uint32_t state[25], unsigned round);
void sha3llRound1600(uint64_t state[25], unsigned round);
void sha3llTransform(uint64_t state[25]);

// High level
typedef struct CCtxSha3 {
  uint64_t State[25];
  uint32_t HashSize;
  uint32_t BlockSize;
  uint32_t BufferSize;
} CCtxSha3;

void sha3Init(CCtxSha3 *ctx, unsigned hashSize);
void sha3Update(CCtxSha3 *ctx, const void *data, size_t size);
void sha3Final(CCtxSha3 *ctx, uint8_t *hash, int isKeccak);
void sha3(const void *data, size_t size, uint8_t *hash, unsigned hashSize, int isKeccak);

#ifdef __cplusplus
}
#endif
