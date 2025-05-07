#include "blockmaker/sha3.h"
#include <string.h>

static inline uint64_t rol32(const uint32_t x, const int n)
{
  return (x << n) | (x >> (32 - n));
}

static inline uint64_t rol64(const uint64_t x, const int n)
{
  return (x << n) | (x >> (64 - n));
}

static const uint32_t KeccakConstants800[22] = {
  0x00000001, 0x00008082, 0x0000808a, 0x80008000, 0x0000808b, 0x80000001, 0x80008081,
  0x00008009, 0x0000008a, 0x00000088, 0x80008009, 0x8000000a, 0x8000808b, 0x0000008b,
  0x00008089, 0x00008003, 0x00008002, 0x00000080, 0x0000800a, 0x8000000a, 0x80008081,
  0x00008080,
};

static uint64_t KeccakConstants1600[24] = {
    0x0000000000000001, 0x0000000000008082, 0x800000000000808a,
    0x8000000080008000, 0x000000000000808b, 0x0000000080000001,
    0x8000000080008081, 0x8000000000008009, 0x000000000000008a,
    0x0000000000000088, 0x0000000080008009, 0x000000008000000a,
    0x000000008000808b, 0x800000000000008b, 0x8000000000008089,
    0x8000000000008003, 0x8000000000008002, 0x8000000000000080,
    0x000000000000800a, 0x800000008000000a, 0x8000000080008081,
    0x8000000000008080, 0x0000000080000001, 0x8000000080008008
};

static inline uint64_t chi32(uint32_t a, uint32_t b, uint32_t c)
{
  return a ^ ((~b) & c);
}

static inline uint64_t chi64(uint64_t a, uint64_t b, uint64_t c)
{
  return a ^ ((~b) & c);
}

static inline void memxor(uint8_t *dst, const uint8_t *src, size_t size)
{
  uint64_t *dst64 = (uint64_t*)dst;
  const uint64_t *src64 = (const uint64_t*)src;
  size_t size64 = size / 8;
  for (size_t i = 0; i < size64; i++)
    *dst64++ ^= *src64++;

  uint8_t *dst8 = (uint8_t*)dst64;
  uint8_t *src8 = (uint8_t*)src64;
  size_t size8 = size % 8;
  for (size_t i = 0; i < size8; i++)
    *dst8++ ^= *src8++;
}

void sha3llRound800(uint32_t state[25], unsigned round)
{
  uint32_t bc[5];

  // Theta
  bc[0] = state[0] ^ state[5] ^ state[10] ^ state[15] ^ state[20];
  bc[1] = state[1] ^ state[6] ^ state[11] ^ state[16] ^ state[21];
  bc[2] = state[2] ^ state[7] ^ state[12] ^ state[17] ^ state[22];
  bc[3] = state[3] ^ state[8] ^ state[13] ^ state[18] ^ state[23];
  bc[4] = state[4] ^ state[9] ^ state[14] ^ state[19] ^ state[24];

  {
    uint64_t v0 = bc[4] ^ rol32(bc[1], 1);
    uint64_t v1 = bc[0] ^ rol32(bc[2], 1);
    uint64_t v2 = bc[1] ^ rol32(bc[3], 1);
    uint64_t v3 = bc[2] ^ rol32(bc[4], 1);
    uint64_t v4 = bc[3] ^ rol32(bc[0], 1);
    state[0] ^= v0;
    state[5] ^= v0;
    state[10] ^= v0;
    state[15] ^= v0;
    state[20] ^= v0;
    state[1] ^= v1;
    state[6] ^= v1;
    state[11] ^= v1;
    state[16] ^= v1;
    state[21] ^= v1;
    state[2] ^= v2;
    state[7] ^= v2;
    state[12] ^= v2;
    state[17] ^= v2;
    state[22] ^= v2;
    state[3] ^= v3 ;
    state[8] ^= v3 ;
    state[13] ^= v3 ;
    state[18] ^= v3 ;
    state[23] ^= v3 ;
    state[4] ^= v4;
    state[9] ^= v4;
    state[14] ^= v4;
    state[19] ^= v4;
    state[24] ^= v4;
  }

  // Rho Pi
  uint64_t s1 = state[1];
  state[1] = rol32(state[6], 44);
  state[6] = rol32(state[9], 20);
  state[9] = rol32(state[22], 61);
  state[22] = rol32(state[14], 39);
  state[14] = rol32(state[20], 18);
  state[20] = rol32(state[2], 62);
  state[2] = rol32(state[12], 43);
  state[12] = rol32(state[13], 25);
  state[13] = rol32(state[19], 8);
  state[19] = rol32(state[23], 56);
  state[23] = rol32(state[15], 41);
  state[15] = rol32(state[4], 27);
  state[4] = rol32(state[24], 14);
  state[24] = rol32(state[21], 2);
  state[21] = rol32(state[8], 55);
  state[8] = rol32(state[16], 45);
  state[16] = rol32(state[5], 36);
  state[5] = rol32(state[3], 28);
  state[3] = rol32(state[18], 21);
  state[18] = rol32(state[17], 15);
  state[17] = rol32(state[11], 10);
  state[11] = rol32(state[7], 6);
  state[7] = rol32(state[10], 3);
  state[10] = rol32(s1, 1);

  //  Chi
  {
    uint64_t v, w;
    v = state[0];
    w = state[1];
    state[0] = chi32(v, w, state[2]);
    state[1] = chi32(w, state[2], state[3]);
    state[2] = chi32(state[2], state[3], state[4]);
    state[3] = chi32(state[3], state[4], v);
    state[4] = chi32(state[4], v, w);

    v = state[5];
    w = state[6];
    state[5] = chi32(v, w, state[7]);
    state[6] = chi32(w, state[7], state[8]);
    state[7] = chi32(state[7], state[8], state[9]);
    state[8] = chi32(state[8], state[9], v);
    state[9] = chi32(state[9], v, w);

    v = state[10];
    w = state[11];
    state[10] = chi32(v, w, state[12]);
    state[11] = chi32(w, state[12], state[13]);
    state[12] = chi32(state[12], state[13], state[14]);
    state[13] = chi32(state[13], state[14], v);
    state[14] = chi32(state[14], v, w);

    v = state[15];
    w = state[16];
    state[15] = chi32(v, w, state[17]);
    state[16] = chi32(w, state[17], state[18]);
    state[17] = chi32(state[17], state[18], state[19]);
    state[18] = chi32(state[18], state[19], v);
    state[19] = chi32(state[19], v, w);

    v = state[20];
    w = state[21];
    state[20] = chi32(v, w, state[22]);
    state[21] = chi32(w, state[22], state[23]);
    state[22] = chi32(state[22], state[23], state[24]);
    state[23] = chi32(state[23], state[24], v);
    state[24] = chi32(state[24], v, w);
  }

  //  Iota
  state[0] ^= KeccakConstants800[round];
}

void sha3llRound1600(uint64_t state[25], unsigned round)
{
  uint64_t bc[5];

  // Theta
  bc[0] = state[0] ^ state[5] ^ state[10] ^ state[15] ^ state[20];
  bc[1] = state[1] ^ state[6] ^ state[11] ^ state[16] ^ state[21];
  bc[2] = state[2] ^ state[7] ^ state[12] ^ state[17] ^ state[22];
  bc[3] = state[3] ^ state[8] ^ state[13] ^ state[18] ^ state[23];
  bc[4] = state[4] ^ state[9] ^ state[14] ^ state[19] ^ state[24];

  {
    uint64_t v0 = bc[4] ^ rol64(bc[1], 1);
    uint64_t v1 = bc[0] ^ rol64(bc[2], 1);
    uint64_t v2 = bc[1] ^ rol64(bc[3], 1);
    uint64_t v3 = bc[2] ^ rol64(bc[4], 1);
    uint64_t v4 = bc[3] ^ rol64(bc[0], 1);
    state[0] ^= v0;
    state[5] ^= v0;
    state[10] ^= v0;
    state[15] ^= v0;
    state[20] ^= v0;
    state[1] ^= v1;
    state[6] ^= v1;
    state[11] ^= v1;
    state[16] ^= v1;
    state[21] ^= v1;
    state[2] ^= v2;
    state[7] ^= v2;
    state[12] ^= v2;
    state[17] ^= v2;
    state[22] ^= v2;
    state[3] ^= v3 ;
    state[8] ^= v3 ;
    state[13] ^= v3 ;
    state[18] ^= v3 ;
    state[23] ^= v3 ;
    state[4] ^= v4;
    state[9] ^= v4;
    state[14] ^= v4;
    state[19] ^= v4;
    state[24] ^= v4;
  }

  // Rho Pi
  uint64_t s1 = state[1];
  state[1] = rol64(state[6], 44);
  state[6] = rol64(state[9], 20);
  state[9] = rol64(state[22], 61);
  state[22] = rol64(state[14], 39);
  state[14] = rol64(state[20], 18);
  state[20] = rol64(state[2], 62);
  state[2] = rol64(state[12], 43);
  state[12] = rol64(state[13], 25);
  state[13] = rol64(state[19], 8);
  state[19] = rol64(state[23], 56);
  state[23] = rol64(state[15], 41);
  state[15] = rol64(state[4], 27);
  state[4] = rol64(state[24], 14);
  state[24] = rol64(state[21], 2);
  state[21] = rol64(state[8], 55);
  state[8] = rol64(state[16], 45);
  state[16] = rol64(state[5], 36);
  state[5] = rol64(state[3], 28);
  state[3] = rol64(state[18], 21);
  state[18] = rol64(state[17], 15);
  state[17] = rol64(state[11], 10);
  state[11] = rol64(state[7], 6);
  state[7] = rol64(state[10], 3);
  state[10] = rol64(s1, 1);

  //  Chi
  {
    uint64_t v, w;
    v = state[0];
    w = state[1];
    state[0] = chi64(v, w, state[2]);
    state[1] = chi64(w, state[2], state[3]);
    state[2] = chi64(state[2], state[3], state[4]);
    state[3] = chi64(state[3], state[4], v);
    state[4] = chi64(state[4], v, w);

    v = state[5];
    w = state[6];
    state[5] = chi64(v, w, state[7]);
    state[6] = chi64(w, state[7], state[8]);
    state[7] = chi64(state[7], state[8], state[9]);
    state[8] = chi64(state[8], state[9], v);
    state[9] = chi64(state[9], v, w);

    v = state[10];
    w = state[11];
    state[10] = chi64(v, w, state[12]);
    state[11] = chi64(w, state[12], state[13]);
    state[12] = chi64(state[12], state[13], state[14]);
    state[13] = chi64(state[13], state[14], v);
    state[14] = chi64(state[14], v, w);

    v = state[15];
    w = state[16];
    state[15] = chi64(v, w, state[17]);
    state[16] = chi64(w, state[17], state[18]);
    state[17] = chi64(state[17], state[18], state[19]);
    state[18] = chi64(state[18], state[19], v);
    state[19] = chi64(state[19], v, w);

    v = state[20];
    w = state[21];
    state[20] = chi64(v, w, state[22]);
    state[21] = chi64(w, state[22], state[23]);
    state[22] = chi64(state[22], state[23], state[24]);
    state[23] = chi64(state[23], state[24], v);
    state[24] = chi64(state[24], v, w);
  }

  //  Iota
  state[0] ^= KeccakConstants1600[round];
}

void sha3llTransform(uint64_t state[25])
{
  for (unsigned i = 0; i < 24; i++)
    sha3llRound1600(state, i);
}

void sha3Init(CCtxSha3 *ctx, unsigned hashSize)
{
  ctx->HashSize = hashSize;
  ctx->BlockSize = 200 - 2 * hashSize;
  ctx->BufferSize = 0;
  memset(&ctx->State, 0, sizeof(ctx->State));
}

void sha3Update(CCtxSha3 *ctx, const void *data, size_t size)
{
  size_t remaining = size;
  const uint8_t *p = (const uint8_t*)data;
  uint8_t *state8 = (uint8_t*)ctx->State;
  if (ctx->BufferSize) {
    // step 1: copy remainder and transform if need
    size_t maxSize = ctx->BlockSize - ctx->BufferSize;
    size_t copySize = maxSize <= remaining ? maxSize : remaining;
    memxor(state8 + ctx->BufferSize, data, copySize);
    ctx->BufferSize += copySize;
    remaining -= copySize;
    p += copySize;
    if (ctx->BufferSize == ctx->BlockSize) {
      sha3llTransform(ctx->State);
      ctx->BufferSize = 0;
    }
  }

  if (remaining) {
    // step 2: full rounds
    size_t fullRounds = remaining / ctx->BlockSize;
    for (size_t i = 0; i < fullRounds; i++) {
      memxor(state8, p, ctx->BlockSize);
      sha3llTransform(ctx->State);
      p += ctx->BlockSize;
    }
    remaining -= fullRounds * ctx->BlockSize;

    // step 3: copy remainder
    memxor(state8, p, remaining);
    ctx->BufferSize = remaining;
  }
}

void sha3Final(CCtxSha3 *ctx, uint8_t *hash, int isKeccak)
{
  // anyway single sha3 transform here
  uint8_t *state8 = (uint8_t*)ctx->State;
  state8[ctx->BufferSize] ^= !isKeccak ? 0x06 : 0x01;
  state8[ctx->BlockSize - 1] ^= 0x80;
  sha3llTransform(ctx->State);
  memcpy(hash, ctx->State, ctx->HashSize);
}

void sha3(const void *data, size_t size, uint8_t *hash, unsigned hashSize, int isKeccak)
{
  uint64_t state[25];
  uint8_t *state8 = (uint8_t*)state;
  unsigned blockSize = 200 - 2 * hashSize;
  const uint8_t *p = (const uint8_t*)data;
  memset(state, 0, sizeof(state));

  // perform full rounds
  {
    size_t roundsNum = size / blockSize;
    for (unsigned i = 0; i < roundsNum; i++) {
      memxor(state8, p, blockSize);
      sha3llTransform(state);
      p += blockSize;
    }
  }

  // final
  unsigned bufferSize = size % blockSize;
  memxor(state8, p, bufferSize);
  state8[bufferSize] ^= !isKeccak ? 0x06 : 0x01;
  state8[blockSize - 1] ^= 0x80;
  sha3llTransform(state);
  memcpy(hash, state, hashSize);
}
