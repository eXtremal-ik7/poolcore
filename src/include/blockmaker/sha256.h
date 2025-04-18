#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Low level
void sha256llInit(uint32_t state[8]);
void sha256llTransform(uint32_t state[8], const uint32_t in[16], int bswap);
void sha256llFinal(const uint32_t state[8], uint8_t *hash, int bswap);

// High level
typedef struct CCtxSha256 {
  uint32_t state[16];
  uint8_t buffer[64];
  uint32_t bufferSize;
  size_t MsgSize;
} CCtxSha256;

void sha256Init(CCtxSha256 *ctx);
void sha256Update(CCtxSha256 *ctx, const void *data, size_t size);
void sha256Final(CCtxSha256 *ctx, uint8_t *hash);
void sha256(const void *data, size_t size, uint8_t *hash);

#ifdef __cplusplus
}
#endif
