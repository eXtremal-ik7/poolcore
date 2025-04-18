#include "blockmaker/sha256.h"
#include <string.h>
#ifdef LIBPOW_SHANI_ENABLED
#include <immintrin.h>
#endif

static uint32_t __sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline uint32_t rol32(const uint32_t x, const int n)
{
  return (x << n) | (x >> (32 - n));
}

static inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z)  { return (x & y) ^ (~x & z); }
static inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }

static inline uint32_t ep0(uint32_t x) { return rol32(x, 30) ^ rol32(x, 19) ^ rol32(x, 10); }
static inline uint32_t ep1(uint32_t x) { return rol32(x, 26) ^ rol32(x, 21) ^ rol32(x, 7); }
static inline uint32_t sig0(uint32_t x) { return rol32(x, 25) ^ rol32(x, 14) ^ (x >> 3); }
static inline uint32_t sig1(uint32_t x) { return rol32(x, 15) ^ rol32(x, 13) ^ (x >> 10); }

static inline void sha256_round(uint32_t a, uint32_t b, uint32_t c, uint32_t *d, uint32_t e, uint32_t f, uint32_t g, uint32_t *h, uint32_t x, uint32_t K)
{
  uint32_t t1 = *h + ep1(e) + ch(e, f, g) + K + x;
  uint32_t t2 = ep0(a) + maj(a, b, c);
  *d += t1;
  *h = t1 + t2;
}

static inline void sha256TransformGeneric(uint32_t state[8], const uint32_t in[16], int bswap)
{
  uint32_t A, B, C, D, E, F, G, H;

  uint32_t data[16];
  if (bswap) {
    for (unsigned i = 0; i < 16; i++) {
      data[i] = __builtin_bswap32(in[i]);
    }
  } else {
    memcpy(data, in, 64);
  }

  A = state[0];
  B = state[1];
  C = state[2];
  D = state[3];
  E = state[4];
  F = state[5];
  G = state[6];
  H = state[7];

  sha256_round(A, B, C, &D, E, F, G, &H, data[0], 0x428a2f98);
  sha256_round(H, A, B, &C, D, E, F, &G, data[1], 0x71374491);
  sha256_round(G, H, A, &B, C, D, E, &F, data[2], 0xb5c0fbcf);
  sha256_round(F, G, H, &A, B, C, D, &E, data[3], 0xe9b5dba5);
  sha256_round(E, F, G, &H, A, B, C, &D, data[4], 0x3956c25b);
  sha256_round(D, E, F, &G, H, A, B, &C, data[5], 0x59f111f1);
  sha256_round(C, D, E, &F, G, H, A, &B, data[6], 0x923f82a4);
  sha256_round(B, C, D, &E, F, G, H, &A, data[7], 0xab1c5ed5);
  sha256_round(A, B, C, &D, E, F, G, &H, data[8], 0xd807aa98);
  sha256_round(H, A, B, &C, D, E, F, &G, data[9], 0x12835b01);
  sha256_round(G, H, A, &B, C, D, E, &F, data[10], 0x243185be);
  sha256_round(F, G, H, &A, B, C, D, &E, data[11], 0x550c7dc3);
  sha256_round(E, F, G, &H, A, B, C, &D, data[12], 0x72be5d74);
  sha256_round(D, E, F, &G, H, A, B, &C, data[13], 0x80deb1fe);
  sha256_round(C, D, E, &F, G, H, A, &B, data[14], 0x9bdc06a7);
  sha256_round(B, C, D, &E, F, G, H, &A, data[15], 0xc19bf174);

  for (unsigned i = 0; i < 16; i++)
    data[i] += (sig1(data[(i+14) % 16]) + data[(i+9) % 16] + sig0(data[(i+1) % 16]));
  sha256_round(A, B, C, &D, E, F, G, &H, data[0], 0xe49b69c1);
  sha256_round(H, A, B, &C, D, E, F, &G, data[1], 0xefbe4786);
  sha256_round(G, H, A, &B, C, D, E, &F, data[2], 0x0fc19dc6);
  sha256_round(F, G, H, &A, B, C, D, &E, data[3], 0x240ca1cc);
  sha256_round(E, F, G, &H, A, B, C, &D, data[4], 0x2de92c6f);
  sha256_round(D, E, F, &G, H, A, B, &C, data[5], 0x4a7484aa);
  sha256_round(C, D, E, &F, G, H, A, &B, data[6], 0x5cb0a9dc);
  sha256_round(B, C, D, &E, F, G, H, &A, data[7], 0x76f988da);
  sha256_round(A, B, C, &D, E, F, G, &H, data[8], 0x983e5152);
  sha256_round(H, A, B, &C, D, E, F, &G, data[9], 0xa831c66d);
  sha256_round(G, H, A, &B, C, D, E, &F, data[10], 0xb00327c8);
  sha256_round(F, G, H, &A, B, C, D, &E, data[11], 0xbf597fc7);
  sha256_round(E, F, G, &H, A, B, C, &D, data[12], 0xc6e00bf3);
  sha256_round(D, E, F, &G, H, A, B, &C, data[13], 0xd5a79147);
  sha256_round(C, D, E, &F, G, H, A, &B, data[14], 0x06ca6351);
  sha256_round(B, C, D, &E, F, G, H, &A, data[15], 0x14292967);

  for (unsigned i = 0; i < 16; i++)
    data[i] += (sig1(data[(i+14) % 16]) + data[(i+9) % 16] + sig0(data[(i+1) % 16]));
  sha256_round(A, B, C, &D, E, F, G, &H, data[0], 0x27b70a85);
  sha256_round(H, A, B, &C, D, E, F, &G, data[1], 0x2e1b2138);
  sha256_round(G, H, A, &B, C, D, E, &F, data[2], 0x4d2c6dfc);
  sha256_round(F, G, H, &A, B, C, D, &E, data[3], 0x53380d13);
  sha256_round(E, F, G, &H, A, B, C, &D, data[4], 0x650a7354);
  sha256_round(D, E, F, &G, H, A, B, &C, data[5], 0x766a0abb);
  sha256_round(C, D, E, &F, G, H, A, &B, data[6], 0x81c2c92e);
  sha256_round(B, C, D, &E, F, G, H, &A, data[7], 0x92722c85);
  sha256_round(A, B, C, &D, E, F, G, &H, data[8], 0xa2bfe8a1);
  sha256_round(H, A, B, &C, D, E, F, &G, data[9], 0xa81a664b);
  sha256_round(G, H, A, &B, C, D, E, &F, data[10], 0xc24b8b70);
  sha256_round(F, G, H, &A, B, C, D, &E, data[11], 0xc76c51a3);
  sha256_round(E, F, G, &H, A, B, C, &D, data[12], 0xd192e819);
  sha256_round(D, E, F, &G, H, A, B, &C, data[13], 0xd6990624);
  sha256_round(C, D, E, &F, G, H, A, &B, data[14], 0xf40e3585);
  sha256_round(B, C, D, &E, F, G, H, &A, data[15], 0x106aa070);

  for (unsigned i = 0; i < 16; i++)
    data[i] += (sig1(data[(i+14) % 16]) + data[(i+9) % 16] + sig0(data[(i+1) % 16]));
  sha256_round(A, B, C, &D, E, F, G, &H, data[0], 0x19a4c116);
  sha256_round(H, A, B, &C, D, E, F, &G, data[1], 0x1e376c08);
  sha256_round(G, H, A, &B, C, D, E, &F, data[2], 0x2748774c);
  sha256_round(F, G, H, &A, B, C, D, &E, data[3], 0x34b0bcb5);
  sha256_round(E, F, G, &H, A, B, C, &D, data[4], 0x391c0cb3);
  sha256_round(D, E, F, &G, H, A, B, &C, data[5], 0x4ed8aa4a);
  sha256_round(C, D, E, &F, G, H, A, &B, data[6], 0x5b9cca4f);
  sha256_round(B, C, D, &E, F, G, H, &A, data[7], 0x682e6ff3);
  sha256_round(A, B, C, &D, E, F, G, &H, data[8], 0x748f82ee);
  sha256_round(H, A, B, &C, D, E, F, &G, data[9], 0x78a5636f);
  sha256_round(G, H, A, &B, C, D, E, &F, data[10], 0x84c87814);
  sha256_round(F, G, H, &A, B, C, D, &E, data[11], 0x8cc70208);
  sha256_round(E, F, G, &H, A, B, C, &D, data[12], 0x90befffa);
  sha256_round(D, E, F, &G, H, A, B, &C, data[13], 0xa4506ceb);
  sha256_round(C, D, E, &F, G, H, A, &B, data[14], 0xbef9a3f7);
  sha256_round(B, C, D, &E, F, G, H, &A, data[15], 0xc67178f2);

  state[0] += A;
  state[1] += B;
  state[2] += C;
  state[3] += D;
  state[4] += E;
  state[5] += F;
  state[6] += G;
  state[7] += H;
}

#ifdef LIBPOW_SHANI_ENABLED
static inline void sha256TransformShaNI(uint32_t state[8], const uint32_t in[16], int bswap)
{ 
  const __m128i* k128 = (const __m128i*)(__sha256_k);
  const __m128i* in128 = (const __m128i*)(in);

  __m128i state0 = _mm_loadu_si128((__m128i*)(&state[0]));
  __m128i state1 = _mm_loadu_si128((__m128i*)(&state[4]));

  state0 = _mm_shuffle_epi32(state0, 0xB1); // CDAB
  state1 = _mm_shuffle_epi32(state1, 0x1B); // EFGH

  __m128i t = _mm_alignr_epi8(state0, state1, 8); // ABEF
  state1 = _mm_blend_epi16(state1, state0, 0xF0); // CDGH
  state0 = t;

  // Save current state
  const __m128i abefOld = state0;
  const __m128i cdghOld = state1;

  __m128i msg;
  __m128i msg0;
  __m128i msg1;
  __m128i msg2;
  __m128i msg3;
  if (bswap) {
    const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0b, 0x0405060700010203);
    msg0 = _mm_shuffle_epi8(_mm_loadu_si128(in128 + 0), mask);
    msg1 = _mm_shuffle_epi8(_mm_loadu_si128(in128 + 1), mask);
    msg2 = _mm_shuffle_epi8(_mm_loadu_si128(in128 + 2), mask);
    msg3 = _mm_shuffle_epi8(_mm_loadu_si128(in128 + 3), mask);
  } else {
    msg0 = _mm_loadu_si128(in128 + 0);
    msg1 = _mm_loadu_si128(in128 + 1);
    msg2 = _mm_loadu_si128(in128 + 2);
    msg3 = _mm_loadu_si128(in128 + 3);
  }

  // Rounds 0-3
  msg = _mm_add_epi32(msg0, _mm_load_si128(k128));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  // Rounds 4-7
  msg = _mm_add_epi32(msg1, _mm_load_si128(k128 + 1));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg0 = _mm_sha256msg1_epu32(msg0, msg1);
  // Rounds 8-11
  msg = _mm_add_epi32(msg2, _mm_load_si128(k128 + 2));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg1 = _mm_sha256msg1_epu32(msg1, msg2);
  // Rounds 12-15
  msg = _mm_add_epi32(msg3, _mm_load_si128(k128 + 3));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg0 = _mm_add_epi32(msg0, _mm_alignr_epi8(msg3, msg2, 4));
  msg0 = _mm_sha256msg2_epu32(msg0, msg3);
  msg2 = _mm_sha256msg1_epu32(msg2, msg3);
  // Rounds 16-19
  msg = _mm_add_epi32(msg0, _mm_load_si128(k128 + 4));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg1 = _mm_add_epi32(msg1, _mm_alignr_epi8(msg0, msg3, 4));
  msg1 = _mm_sha256msg2_epu32(msg1, msg0);
  msg3 = _mm_sha256msg1_epu32(msg3, msg0);
  // Rounds 20-23
  msg = _mm_add_epi32(msg1, _mm_load_si128(k128 + 5));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg2 = _mm_add_epi32(msg2, _mm_alignr_epi8(msg1, msg0, 4));
  msg2 = _mm_sha256msg2_epu32(msg2, msg1);
  msg0 = _mm_sha256msg1_epu32(msg0, msg1);
  // Rounds 24-27
  msg = _mm_add_epi32(msg2, _mm_load_si128(k128 + 6));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg3 = _mm_add_epi32(msg3, _mm_alignr_epi8(msg2, msg1, 4));
  msg3 = _mm_sha256msg2_epu32(msg3, msg2);
  msg1 = _mm_sha256msg1_epu32(msg1, msg2);
  // Rounds 28-31
  msg = _mm_add_epi32(msg3, _mm_load_si128(k128 + 7));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg0 = _mm_add_epi32(msg0, _mm_alignr_epi8(msg3, msg2, 4));
  msg0 = _mm_sha256msg2_epu32(msg0, msg3);
  msg2 = _mm_sha256msg1_epu32(msg2, msg3);
  // Rounds 32-35
  msg = _mm_add_epi32(msg0, _mm_load_si128(k128 + 8));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg1 = _mm_add_epi32(msg1, _mm_alignr_epi8(msg0, msg3, 4));
  msg1 = _mm_sha256msg2_epu32(msg1, msg0);
  msg3 = _mm_sha256msg1_epu32(msg3, msg0);
  // Rounds 36-39
  msg = _mm_add_epi32(msg1, _mm_load_si128(k128 + 9));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg2 = _mm_add_epi32(msg2, _mm_alignr_epi8(msg1, msg0, 4));
  msg2 = _mm_sha256msg2_epu32(msg2, msg1);
  msg0 = _mm_sha256msg1_epu32(msg0, msg1);
  // Rounds 40-43
  msg = _mm_add_epi32(msg2, _mm_load_si128(k128 + 10));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg3 = _mm_add_epi32(msg3, _mm_alignr_epi8(msg2, msg1, 4));
  msg3 = _mm_sha256msg2_epu32(msg3, msg2);
  msg1 = _mm_sha256msg1_epu32(msg1, msg2);
  // Rounds 44-47
  msg = _mm_add_epi32(msg3, _mm_load_si128(k128 + 11));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg0 = _mm_add_epi32(msg0, _mm_alignr_epi8(msg3, msg2, 4));
  msg0 = _mm_sha256msg2_epu32(msg0, msg3);
  msg2 = _mm_sha256msg1_epu32(msg2, msg3);
  // Rounds 48-51
  msg = _mm_add_epi32(msg0, _mm_load_si128(k128 + 12));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg1 = _mm_add_epi32(msg1, _mm_alignr_epi8(msg0, msg3, 4));
  msg1 = _mm_sha256msg2_epu32(msg1, msg0);
  msg3 = _mm_sha256msg1_epu32(msg3, msg0);
  // Rounds 52-55
  msg = _mm_add_epi32(msg1, _mm_load_si128(k128 + 13));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg2 = _mm_add_epi32(msg2, _mm_alignr_epi8(msg1, msg0, 4));
  msg2 = _mm_sha256msg2_epu32(msg2, msg1);
  // Rounds 56-59
  msg = _mm_add_epi32(msg2, _mm_load_si128(k128 + 14));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));
  msg3 = _mm_add_epi32(msg3, _mm_alignr_epi8(msg2, msg1, 4));
  msg3 = _mm_sha256msg2_epu32(msg3, msg2);
  // Rounds 60-63
  msg = _mm_add_epi32(msg3, _mm_load_si128(k128 + 15));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(msg, 0x0E));

  // Add values back to state
  state0 = _mm_add_epi32(state0, abefOld);
  state1 = _mm_add_epi32(state1, cdghOld);
  state0 = _mm_shuffle_epi32(state0, 0x1B); // FEBA
  state1 = _mm_shuffle_epi32(state1, 0xB1); // DCHG

  // Save state
  _mm_storeu_si128((__m128i*)(&state[0]), _mm_blend_epi16(state0, state1, 0xF0)); // DCBA
  _mm_storeu_si128((__m128i*)(&state[4]), _mm_alignr_epi8(state1, state0, 8)); // ABEF
}
#endif

void sha256llInit(uint32_t state[8])
{
  state[0] = 0x6a09e667;
  state[1] = 0xbb67ae85;
  state[2] = 0x3c6ef372;
  state[3] = 0xa54ff53a;
  state[4] = 0x510e527f;
  state[5] = 0x9b05688c;
  state[6] = 0x1f83d9ab;
  state[7] = 0x5be0cd19;
}

void sha256llTransform(uint32_t state[8], const uint32_t in[16], int bswap)
{
#ifdef LIBPOW_SHANI_ENABLED
  sha256TransformShaNI(state, in, bswap);
#else
  sha256TransformGeneric(state, in, bswap);
#endif
}

void sha256llFinal(const uint32_t state[8], uint8_t *hash, int bswap)
{
  uint32_t *hash32 = (uint32_t*)hash;
  if (bswap) {
    for (unsigned i = 0; i < 8; i++)
      hash32[i] = __builtin_bswap32(state[i]);
  } else {
    memcpy(hash, state, 32);
  }
}

void sha256Init(CCtxSha256 *ctx)
{
  sha256llInit(ctx->state);
  ctx->bufferSize = 0;
  ctx->MsgSize = 0;
}

void sha256Update(CCtxSha256 *ctx, const void *data, size_t size)
{
  size_t remaining = size;
  const uint8_t *p = (const uint8_t*)data;

  {
    // step 1: copy remainder and transform if need
    size_t maxSize = 64 - ctx->bufferSize;
    size_t copySize = maxSize <= remaining ? maxSize : remaining;
    memcpy(ctx->buffer + ctx->bufferSize, data, copySize);
    ctx->bufferSize += copySize;
    remaining -= copySize;
    p += copySize;
    if (ctx->bufferSize == 64) {
      sha256llTransform(ctx->state, (const uint32_t*)ctx->buffer, 1);
      ctx->bufferSize = 0;
    }
  }

  if (remaining) {
    // step 2: full rounds
    size_t fullRounds = remaining / 64;
    for (size_t i = 0; i < fullRounds; i++) {
      sha256llTransform(ctx->state, (const uint32_t*)p, 1);
      p += 64;
    }
    remaining -= fullRounds * 64;

    // step 3: copy remainder
    memcpy(ctx->buffer, p, remaining);
    ctx->bufferSize = remaining;
  }

  ctx->MsgSize += size;
}

void sha256Final(CCtxSha256 *ctx, uint8_t *hash)
{
  // we assume that message is 8-bit rounded
  // padding size is 9 bytes (0x80 + message length in bits)
  //   80 xx xx xx xx xx xx xx xx
  ctx->buffer[ctx->bufferSize] = 0x80;
  if (ctx->bufferSize > 64 - 1 - 8) {
    // fill remaining buffer with zeroes and transform
    memset(ctx->buffer + ctx->bufferSize + 1, 0, 64 - (ctx->bufferSize + 1));
    sha256llTransform(ctx->state, (const uint32_t*)ctx->buffer, 1);
    // last transform only with message length
    uint64_t bitsBigEndian = __builtin_bswap64(ctx->MsgSize * 8);
    uint32_t lastMsg[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, bitsBigEndian, bitsBigEndian >> 32};
    sha256llTransform(ctx->state, lastMsg, 1);
  } else {
    memset(ctx->buffer + ctx->bufferSize + 1, 0, 56 - (ctx->bufferSize + 1));
    *(uint64_t*)(ctx->buffer + 56) = __builtin_bswap64(ctx->MsgSize * 8);
    sha256llTransform(ctx->state, (const uint32_t*)ctx->buffer, 1);
  }

  sha256llFinal(ctx->state, hash, 1);
}

void sha256(const void *data, size_t size, uint8_t *hash)
{
  uint32_t state[8];
  sha256llInit(state);
  const uint8_t *p = (const uint8_t*)data;

  // perform full rounds
  {
    size_t roundsNum = size / 64;
    for (unsigned i = 0; i < roundsNum; i++) {
      sha256llTransform(state, (const uint32_t*)p, 1);
      p += 64;
    }
  }

  // final
  union {
    uint8_t b8[64];
    uint32_t b32[16];
    uint64_t b64[8];
  } buffer;

  unsigned bufferSize = size % 64;
  memset(buffer.b8, 0, sizeof(buffer.b8));
  memcpy(buffer.b8, p, bufferSize);
  buffer.b8[bufferSize] = 0x80;
  uint64_t bitsBigEndian = __builtin_bswap64(size * 8);
  if (bufferSize > 64 - 1 - 8) {
    // fill remaining buffer with zeroes and transform
    sha256llTransform(state, buffer.b32, 1);
    // last transform only with message length
    uint32_t lastMsg[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, bitsBigEndian, bitsBigEndian >> 32};
    sha256llTransform(state, (const uint32_t*)lastMsg, 1);
  } else {
    buffer.b64[7] = bitsBigEndian;
    sha256llTransform(state, buffer.b32, 1);
  }

  sha256llFinal(state, hash, 1);
}
