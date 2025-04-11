#include "blockmaker/scrypt.h"
#include "blockmaker/sha256.h"
#include <stddef.h>
#include <string.h>
#ifdef LIBPOW_SSE2_ENABLED
#include <immintrin.h>
#endif

typedef struct HMAC_SHA256Context {
  CCtxSha256 ictx;
  CCtxSha256 octx;
} HMAC_SHA256_CTX;

static inline void be32enc(void *pp, uint32_t x) { *((uint32_t*)pp) = __builtin_bswap32(x); }
static inline uint32_t le32dec(const void *pp) { return *(uint32_t*)pp; }
static inline void le32enc(void *pp, uint32_t x) { *((uint32_t*)pp) = x; }

// Initialize an HMAC-SHA256 operation with the given key.
static void HMAC_SHA256_Init(HMAC_SHA256_CTX *ctx, const void *_K, size_t Klen)
{
  unsigned char pad[64];
  unsigned char khash[32];
  const unsigned char *K = (const unsigned char *)_K;
  size_t i;

  // If Klen > 64, the key is really SHA256(K)
  if (Klen > 64) {
    sha256Init(&ctx->ictx);
    sha256Update(&ctx->ictx, K, Klen);
    sha256Final(&ctx->ictx, khash);
    K = khash;
    Klen = 32;
  }

  // Inner SHA256 operation is SHA256(K xor [block of 0x36] || data)
  sha256Init(&ctx->ictx);
  memset(pad, 0x36, 64);
  for (i = 0; i < Klen; i++)
    pad[i] ^= K[i];
  sha256Update(&ctx->ictx, pad, 64);

  // Outer SHA256 operation is SHA256(K xor [block of 0x5c] || hash)
  sha256Init(&ctx->octx);
  memset(pad, 0x5c, 64);
  for (i = 0; i < Klen; i++)
    pad[i] ^= K[i];
  sha256Update(&ctx->octx, pad, 64);
}

// Add bytes to the HMAC-SHA256 operation
static void HMAC_SHA256_Update(HMAC_SHA256_CTX *ctx, const void *in, size_t len)
{
  // Feed data to the inner SHA256 operation
  sha256Update(&ctx->ictx, in, len);
}

// Finish an HMAC-SHA256 operation
static void HMAC_SHA256_Final(unsigned char digest[32], HMAC_SHA256_CTX *ctx)
{
  unsigned char ihash[32];
  // Finish the inner SHA256 operation
  sha256Final(&ctx->ictx, ihash);
  // Feed the inner hash to the outer SHA256 operation
  sha256Update(&ctx->octx, ihash, 32);
  // Finish the outer SHA256 operation
  sha256Final(&ctx->octx, digest);
}

static void PBKDF2_SHA256(const uint8_t *passwd,
                          size_t passwdlen,
                          const uint8_t *salt,
                          size_t saltlen,
                          uint64_t c,
                          uint8_t *buf,
                          size_t dkLen)
{
  HMAC_SHA256_CTX PShctx, hctx;
  size_t i;
  uint8_t ivec[4];
  uint8_t U[32];
  uint8_t T[32];
  uint64_t j;
  int k;
  size_t clen;

  // Compute HMAC state after processing P and S
  HMAC_SHA256_Init(&PShctx, passwd, passwdlen);
  HMAC_SHA256_Update(&PShctx, salt, saltlen);

  // Iterate through the blocks
  for (i = 0; i * 32 < dkLen; i++) {
    // Generate INT(i + 1)
    be32enc(ivec, (uint32_t)(i + 1));

    // Compute U_1 = PRF(P, S || INT(i))
    memcpy(&hctx, &PShctx, sizeof(HMAC_SHA256_CTX));
    HMAC_SHA256_Update(&hctx, ivec, 4);
    HMAC_SHA256_Final(U, &hctx);

    // T_i = U_1 ...
    memcpy(T, U, 32);

    for (j = 2; j <= c; j++) {
      // Compute U_j
      HMAC_SHA256_Init(&hctx, passwd, passwdlen);
      HMAC_SHA256_Update(&hctx, U, 32);
      HMAC_SHA256_Final(U, &hctx);

      // ... xor U_j ...
      for (k = 0; k < 32; k++)
        T[k] ^= U[k];
    }

    // Copy as many bytes as necessary into buf
    clen = dkLen - i * 32;
    if (clen > 32)
      clen = 32;
    memcpy(&buf[i * 32], T, clen);
  }
}

static inline uint32_t rol32(const uint32_t x, const int n)
{
  return (x << n) | (x >> (32 - n));
}

static inline void xorSalsa8(uint32_t B[16], const uint32_t Bx[16])
{
  uint32_t x00,x01,x02,x03,x04,x05,x06,x07,x08,x09,x10,x11,x12,x13,x14,x15;
  int i;

  x00 = (B[ 0] ^= Bx[ 0]);
  x01 = (B[ 1] ^= Bx[ 1]);
  x02 = (B[ 2] ^= Bx[ 2]);
  x03 = (B[ 3] ^= Bx[ 3]);
  x04 = (B[ 4] ^= Bx[ 4]);
  x05 = (B[ 5] ^= Bx[ 5]);
  x06 = (B[ 6] ^= Bx[ 6]);
  x07 = (B[ 7] ^= Bx[ 7]);
  x08 = (B[ 8] ^= Bx[ 8]);
  x09 = (B[ 9] ^= Bx[ 9]);
  x10 = (B[10] ^= Bx[10]);
  x11 = (B[11] ^= Bx[11]);
  x12 = (B[12] ^= Bx[12]);
  x13 = (B[13] ^= Bx[13]);
  x14 = (B[14] ^= Bx[14]);
  x15 = (B[15] ^= Bx[15]);

  for (i = 0; i < 8; i += 2) {
    // Operate on columns
    x04 ^= rol32(x00 + x12,  7);  x09 ^= rol32(x05 + x01,  7);
    x14 ^= rol32(x10 + x06,  7);  x03 ^= rol32(x15 + x11,  7);
    x08 ^= rol32(x04 + x00,  9);  x13 ^= rol32(x09 + x05,  9);
    x02 ^= rol32(x14 + x10,  9);  x07 ^= rol32(x03 + x15,  9);
    x12 ^= rol32(x08 + x04, 13);  x01 ^= rol32(x13 + x09, 13);
    x06 ^= rol32(x02 + x14, 13);  x11 ^= rol32(x07 + x03, 13);
    x00 ^= rol32(x12 + x08, 18);  x05 ^= rol32(x01 + x13, 18);
    x10 ^= rol32(x06 + x02, 18);  x15 ^= rol32(x11 + x07, 18);
    // Operate on rows
    x01 ^= rol32(x00 + x03,  7);  x06 ^= rol32(x05 + x04,  7);
    x11 ^= rol32(x10 + x09,  7);  x12 ^= rol32(x15 + x14,  7);
    x02 ^= rol32(x01 + x00,  9);  x07 ^= rol32(x06 + x05,  9);
    x08 ^= rol32(x11 + x10,  9);  x13 ^= rol32(x12 + x15,  9);
    x03 ^= rol32(x02 + x01, 13);  x04 ^= rol32(x07 + x06, 13);
    x09 ^= rol32(x08 + x11, 13);  x14 ^= rol32(x13 + x12, 13);
    x00 ^= rol32(x03 + x02, 18);  x05 ^= rol32(x04 + x07, 18);
    x10 ^= rol32(x09 + x08, 18);  x15 ^= rol32(x14 + x13, 18);
  }

  B[ 0] += x00;
  B[ 1] += x01;
  B[ 2] += x02;
  B[ 3] += x03;
  B[ 4] += x04;
  B[ 5] += x05;
  B[ 6] += x06;
  B[ 7] += x07;
  B[ 8] += x08;
  B[ 9] += x09;
  B[10] += x10;
  B[11] += x11;
  B[12] += x12;
  B[13] += x13;
  B[14] += x14;
  B[15] += x15;
}

#ifdef LIBPOW_SSE2_ENABLED
static inline void xorSalsa8SSE2(__m128i B[4], const __m128i Bx[4])
{
  __m128i X0, X1, X2, X3;
  __m128i T;
  int i;

  X0 = B[0] = _mm_xor_si128(B[0], Bx[0]);
  X1 = B[1] = _mm_xor_si128(B[1], Bx[1]);
  X2 = B[2] = _mm_xor_si128(B[2], Bx[2]);
  X3 = B[3] = _mm_xor_si128(B[3], Bx[3]);

  for (i = 0; i < 8; i += 2) {
    // Operate on "columns"
    T = _mm_add_epi32(X0, X3);
    X1 = _mm_xor_si128(X1, _mm_slli_epi32(T, 7));
    X1 = _mm_xor_si128(X1, _mm_srli_epi32(T, 25));
    T = _mm_add_epi32(X1, X0);
    X2 = _mm_xor_si128(X2, _mm_slli_epi32(T, 9));
    X2 = _mm_xor_si128(X2, _mm_srli_epi32(T, 23));
    T = _mm_add_epi32(X2, X1);
    X3 = _mm_xor_si128(X3, _mm_slli_epi32(T, 13));
    X3 = _mm_xor_si128(X3, _mm_srli_epi32(T, 19));
    T = _mm_add_epi32(X3, X2);
    X0 = _mm_xor_si128(X0, _mm_slli_epi32(T, 18));
    X0 = _mm_xor_si128(X0, _mm_srli_epi32(T, 14));

    // Rearrange data
    X1 = _mm_shuffle_epi32(X1, 0x93);
    X2 = _mm_shuffle_epi32(X2, 0x4E);
    X3 = _mm_shuffle_epi32(X3, 0x39);

    // Operate on "rows"
    T = _mm_add_epi32(X0, X1);
    X3 = _mm_xor_si128(X3, _mm_slli_epi32(T, 7));
    X3 = _mm_xor_si128(X3, _mm_srli_epi32(T, 25));
    T = _mm_add_epi32(X3, X0);
    X2 = _mm_xor_si128(X2, _mm_slli_epi32(T, 9));
    X2 = _mm_xor_si128(X2, _mm_srli_epi32(T, 23));
    T = _mm_add_epi32(X2, X3);
    X1 = _mm_xor_si128(X1, _mm_slli_epi32(T, 13));
    X1 = _mm_xor_si128(X1, _mm_srli_epi32(T, 19));
    T = _mm_add_epi32(X1, X2);
    X0 = _mm_xor_si128(X0, _mm_slli_epi32(T, 18));
    X0 = _mm_xor_si128(X0, _mm_srli_epi32(T, 14));

    // Rearrange data
    X1 = _mm_shuffle_epi32(X1, 0x39);
    X2 = _mm_shuffle_epi32(X2, 0x4E);
    X3 = _mm_shuffle_epi32(X3, 0x93);
  }

  B[0] = _mm_add_epi32(B[0], X0);
  B[1] = _mm_add_epi32(B[1], X1);
  B[2] = _mm_add_epi32(B[2], X2);
  B[3] = _mm_add_epi32(B[3], X3);
}
#endif

void scrypt_1024_1_1_256_sp_generic(const char *input, char *output, char *scratchpad)
{
  uint8_t B[128];
  uint32_t X[32];
  uint32_t *V;
  uint32_t i, j, k;

  V = (uint32_t *)(((uintptr_t)(scratchpad) + 63) & ~ (uintptr_t)(63));

  PBKDF2_SHA256((const uint8_t *)input, 80, (const uint8_t *)input, 80, 1, B, 128);

  for (k = 0; k < 32; k++)
    X[k] = le32dec(&B[4 * k]);

  for (i = 0; i < 1024; i++) {
    memcpy(&V[i * 32], X, 128);
    xorSalsa8(&X[0], &X[16]);
    xorSalsa8(&X[16], &X[0]);
  }
  for (i = 0; i < 1024; i++) {
    j = 32 * (X[16] & 1023);
    for (k = 0; k < 32; k++)
      X[k] ^= V[j + k];
    xorSalsa8(&X[0], &X[16]);
    xorSalsa8(&X[16], &X[0]);
  }

  for (k = 0; k < 32; k++)
    le32enc(&B[4 * k], X[k]);

  PBKDF2_SHA256((const uint8_t *)input, 80, B, 128, 1, (uint8_t *)output, 32);
}

#ifdef LIBPOW_SSE2_ENABLED
void scrypt_1024_1_1_256_sp_sse2(const char *input, char *output, char *scratchpad)
{
  uint8_t B[128];
  union {
    __m128i i128[8];
    uint32_t u32[32];
  } X;
  __m128i *V;
  uint32_t i, j, k;

  V = (__m128i *)(((uintptr_t)(scratchpad) + 63) & ~ (uintptr_t)(63));

  PBKDF2_SHA256((const uint8_t *)input, 80, (const uint8_t *)input, 80, 1, B, 128);

  for (k = 0; k < 2; k++) {
    for (i = 0; i < 16; i++) {
      X.u32[k * 16 + i] = le32dec(&B[(k * 16 + (i * 5 % 16)) * 4]);
    }
  }

  for (i = 0; i < 1024; i++) {
    for (k = 0; k < 8; k++)
      V[i * 8 + k] = X.i128[k];
    xorSalsa8SSE2(&X.i128[0], &X.i128[4]);
    xorSalsa8SSE2(&X.i128[4], &X.i128[0]);
  }
  for (i = 0; i < 1024; i++) {
    j = 8 * (X.u32[16] & 1023);
    for (k = 0; k < 8; k++)
      X.i128[k] = _mm_xor_si128(X.i128[k], V[j + k]);
    xorSalsa8SSE2(&X.i128[0], &X.i128[4]);
    xorSalsa8SSE2(&X.i128[4], &X.i128[0]);
  }

  for (k = 0; k < 2; k++) {
    for (i = 0; i < 16; i++) {
      le32enc(&B[(k * 16 + (i * 5 % 16)) * 4], X.u32[k * 16 + i]);
    }
  }

  PBKDF2_SHA256((const uint8_t *)input, 80, B, 128, 1, (uint8_t *)output, 32);
}
#endif

void scrypt_1024_1_1_256(const void *input, uint8_t output[32])
{
  char scratchpad[LTC_SCRATCHPAD_SIZE];
#ifdef LIBPOW_SSE2_ENABLED
  scrypt_1024_1_1_256_sp_sse2(input, (char*)output, scratchpad);
#else
  scrypt_1024_1_1_256_sp_generic(input, (char*)output, scratchpad);
#endif
}
