#include <stdint.h>
#include <immintrin.h>

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

static inline uint32_t bswap32(uint32_t x)
{
  return __builtin_bswap32(x);
}

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

static inline void sha256Init(uint32_t state[8])
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

static inline void sha256Round(uint32_t &a, uint32_t &b, uint32_t &c, uint32_t &d, uint32_t &e, uint32_t &f, uint32_t &g, uint32_t &h, uint32_t x, uint32_t K)
{
  uint32_t t1 = h + ep1(e) + ch(e, f, g) + K + x;
  uint32_t t2 = ep0(a) + maj(a, b, c);
  d += t1;
  h = t1 + t2;
}

static inline void sha256Transform(uint32_t *state, const uint32_t *in)
{
  uint32_t A, B, C, D, E, F, G, H;

  uint32_t data[16];
#pragma unroll
  for (unsigned i = 0; i < 16; i++)
    data[i] = in[i];

  A = state[0];
  B = state[1];
  C = state[2];
  D = state[3];
  E = state[4];
  F = state[5];
  G = state[6];
  H = state[7];

  sha256Round(A, B, C, D, E, F, G, H, data[0], 0x428a2f98);
  sha256Round(H, A, B, C, D, E, F, G, data[1], 0x71374491);
  sha256Round(G, H, A, B, C, D, E, F, data[2], 0xb5c0fbcf);
  sha256Round(F, G, H, A, B, C, D, E, data[3], 0xe9b5dba5);
  sha256Round(E, F, G, H, A, B, C, D, data[4], 0x3956c25b);
  sha256Round(D, E, F, G, H, A, B, C, data[5], 0x59f111f1);
  sha256Round(C, D, E, F, G, H, A, B, data[6], 0x923f82a4);
  sha256Round(B, C, D, E, F, G, H, A, data[7], 0xab1c5ed5);
  sha256Round(A, B, C, D, E, F, G, H, data[8], 0xd807aa98);
  sha256Round(H, A, B, C, D, E, F, G, data[9], 0x12835b01);
  sha256Round(G, H, A, B, C, D, E, F, data[10], 0x243185be);
  sha256Round(F, G, H, A, B, C, D, E, data[11], 0x550c7dc3);
  sha256Round(E, F, G, H, A, B, C, D, data[12], 0x72be5d74);
  sha256Round(D, E, F, G, H, A, B, C, data[13], 0x80deb1fe);
  sha256Round(C, D, E, F, G, H, A, B, data[14], 0x9bdc06a7);
  sha256Round(B, C, D, E, F, G, H, A, data[15], 0xc19bf174);

#pragma unroll
  for (unsigned i = 0; i < 16; i++)
    data[i] += (sig1(data[(i+14) % 16]) + data[(i+9) % 16] + sig0(data[(i+1) % 16]));
  sha256Round(A, B, C, D, E, F, G, H, data[0], 0xe49b69c1);
  sha256Round(H, A, B, C, D, E, F, G, data[1], 0xefbe4786);
  sha256Round(G, H, A, B, C, D, E, F, data[2], 0x0fc19dc6);
  sha256Round(F, G, H, A, B, C, D, E, data[3], 0x240ca1cc);
  sha256Round(E, F, G, H, A, B, C, D, data[4], 0x2de92c6f);
  sha256Round(D, E, F, G, H, A, B, C, data[5], 0x4a7484aa);
  sha256Round(C, D, E, F, G, H, A, B, data[6], 0x5cb0a9dc);
  sha256Round(B, C, D, E, F, G, H, A, data[7], 0x76f988da);
  sha256Round(A, B, C, D, E, F, G, H, data[8], 0x983e5152);
  sha256Round(H, A, B, C, D, E, F, G, data[9], 0xa831c66d);
  sha256Round(G, H, A, B, C, D, E, F, data[10], 0xb00327c8);
  sha256Round(F, G, H, A, B, C, D, E, data[11], 0xbf597fc7);
  sha256Round(E, F, G, H, A, B, C, D, data[12], 0xc6e00bf3);
  sha256Round(D, E, F, G, H, A, B, C, data[13], 0xd5a79147);
  sha256Round(C, D, E, F, G, H, A, B, data[14], 0x06ca6351);
  sha256Round(B, C, D, E, F, G, H, A, data[15], 0x14292967);

#pragma unroll
  for (unsigned i = 0; i < 16; i++)
    data[i] += (sig1(data[(i+14) % 16]) + data[(i+9) % 16] + sig0(data[(i+1) % 16]));
  sha256Round(A, B, C, D, E, F, G, H, data[0], 0x27b70a85);
  sha256Round(H, A, B, C, D, E, F, G, data[1], 0x2e1b2138);
  sha256Round(G, H, A, B, C, D, E, F, data[2], 0x4d2c6dfc);
  sha256Round(F, G, H, A, B, C, D, E, data[3], 0x53380d13);
  sha256Round(E, F, G, H, A, B, C, D, data[4], 0x650a7354);
  sha256Round(D, E, F, G, H, A, B, C, data[5], 0x766a0abb);
  sha256Round(C, D, E, F, G, H, A, B, data[6], 0x81c2c92e);
  sha256Round(B, C, D, E, F, G, H, A, data[7], 0x92722c85);
  sha256Round(A, B, C, D, E, F, G, H, data[8], 0xa2bfe8a1);
  sha256Round(H, A, B, C, D, E, F, G, data[9], 0xa81a664b);
  sha256Round(G, H, A, B, C, D, E, F, data[10], 0xc24b8b70);
  sha256Round(F, G, H, A, B, C, D, E, data[11], 0xc76c51a3);
  sha256Round(E, F, G, H, A, B, C, D, data[12], 0xd192e819);
  sha256Round(D, E, F, G, H, A, B, C, data[13], 0xd6990624);
  sha256Round(C, D, E, F, G, H, A, B, data[14], 0xf40e3585);
  sha256Round(B, C, D, E, F, G, H, A, data[15], 0x106aa070);

#pragma unroll
  for (unsigned i = 0; i < 16; i++)
    data[i] += (sig1(data[(i+14) % 16]) + data[(i+9) % 16] + sig0(data[(i+1) % 16]));
  sha256Round(A, B, C, D, E, F, G, H, data[0], 0x19a4c116);
  sha256Round(H, A, B, C, D, E, F, G, data[1], 0x1e376c08);
  sha256Round(G, H, A, B, C, D, E, F, data[2], 0x2748774c);
  sha256Round(F, G, H, A, B, C, D, E, data[3], 0x34b0bcb5);
  sha256Round(E, F, G, H, A, B, C, D, data[4], 0x391c0cb3);
  sha256Round(D, E, F, G, H, A, B, C, data[5], 0x4ed8aa4a);
  sha256Round(C, D, E, F, G, H, A, B, data[6], 0x5b9cca4f);
  sha256Round(B, C, D, E, F, G, H, A, data[7], 0x682e6ff3);
  sha256Round(A, B, C, D, E, F, G, H, data[8], 0x748f82ee);
  sha256Round(H, A, B, C, D, E, F, G, data[9], 0x78a5636f);
  sha256Round(G, H, A, B, C, D, E, F, data[10], 0x84c87814);
  sha256Round(F, G, H, A, B, C, D, E, data[11], 0x8cc70208);
  sha256Round(E, F, G, H, A, B, C, D, data[12], 0x90befffa);
  sha256Round(D, E, F, G, H, A, B, C, data[13], 0xa4506ceb);
  sha256Round(C, D, E, F, G, H, A, B, data[14], 0xbef9a3f7);
  sha256Round(B, C, D, E, F, G, H, A, data[15], 0xc67178f2);

  state[0] += A;
  state[1] += B;
  state[2] += C;
  state[3] += D;
  state[4] += E;
  state[5] += F;
  state[6] += G;
  state[7] += H;
}

static inline void sha256InitSHANI(__m128i state[2])
{
  uint32_t stateLocal[8];
  stateLocal[0] = 0x6a09e667;
  stateLocal[1] = 0xbb67ae85;
  stateLocal[2] = 0x3c6ef372;
  stateLocal[3] = 0xa54ff53a;
  stateLocal[4] = 0x510e527f;
  stateLocal[5] = 0x9b05688c;
  stateLocal[6] = 0x1f83d9ab;
  stateLocal[7] = 0x5be0cd19;
  state[0] = _mm_loadu_si128(reinterpret_cast<__m128i*>(&stateLocal[0]));
  state[1] = _mm_loadu_si128(reinterpret_cast<__m128i*>(&stateLocal[4]));
  state[0] = _mm_shuffle_epi32(state[0], 0xB1); // CDAB
  state[1] = _mm_shuffle_epi32(state[1], 0x1B); // EFGH

  __m128i TMP = _mm_alignr_epi8(state[0], state[1], 8); // ABEF
  state[1] = _mm_blend_epi16(state[1], state[0], 0xF0); // CDGH
  state[0] = TMP;
}

static inline void sha256TransformSHANI(__m128i state[2], const __m128i *in)
{
  alignas(64) static const uint32_t K[] = {
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
    0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
    0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
    0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
    0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
    0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
  };

  const __m128i* K_mm = reinterpret_cast<const __m128i*>(K);

  const __m128i old0 = state[0];
  const __m128i old1 = state[1];

  __m128i msg0 = _mm_loadu_si128(in + 0);
  __m128i msg1 = _mm_loadu_si128(in + 1);
  __m128i msg2 = _mm_loadu_si128(in + 2);
  __m128i msg3 = _mm_loadu_si128(in + 3);

  __m128i msg;
  // [0, 3]
  msg = _mm_add_epi32(msg0, _mm_load_si128(K_mm));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  // [4, 7]
  msg = _mm_add_epi32(msg1, _mm_load_si128(K_mm + 1));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg0 = _mm_sha256msg1_epu32(msg0, msg1);
  // [8, 11]
  msg = _mm_add_epi32(msg2, _mm_load_si128(K_mm + 2));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg1 = _mm_sha256msg1_epu32(msg1, msg2);
  // [12, 15]
  msg = _mm_add_epi32(msg3, _mm_load_si128(K_mm + 3));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg0 = _mm_add_epi32(msg0, _mm_alignr_epi8(msg3, msg2, 4));
  msg0 = _mm_sha256msg2_epu32(msg0, msg3);
  msg2 = _mm_sha256msg1_epu32(msg2, msg3);
  // [16, 19]
  msg = _mm_add_epi32(msg0, _mm_load_si128(K_mm + 4));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg1 = _mm_add_epi32(msg1, _mm_alignr_epi8(msg0, msg3, 4));
  msg1 = _mm_sha256msg2_epu32(msg1, msg0);
  msg3 = _mm_sha256msg1_epu32(msg3, msg0);
  // [20, 23]
  msg = _mm_add_epi32(msg1, _mm_load_si128(K_mm + 5));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg2 = _mm_add_epi32(msg2, _mm_alignr_epi8(msg1, msg0, 4));
  msg2 = _mm_sha256msg2_epu32(msg2, msg1);
  msg0 = _mm_sha256msg1_epu32(msg0, msg1);
  // [24, 27]
  msg = _mm_add_epi32(msg2, _mm_load_si128(K_mm + 6));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg3 = _mm_add_epi32(msg3, _mm_alignr_epi8(msg2, msg1, 4));
  msg3 = _mm_sha256msg2_epu32(msg3, msg2);
  msg1 = _mm_sha256msg1_epu32(msg1, msg2);
  // [28, 31]
  msg = _mm_add_epi32(msg3, _mm_load_si128(K_mm + 7));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg0 = _mm_add_epi32(msg0, _mm_alignr_epi8(msg3, msg2, 4));
  msg0 = _mm_sha256msg2_epu32(msg0, msg3);
  msg2 = _mm_sha256msg1_epu32(msg2, msg3);
  // [32, 35]
  msg = _mm_add_epi32(msg0, _mm_load_si128(K_mm + 8));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg1 = _mm_add_epi32(msg1, _mm_alignr_epi8(msg0, msg3, 4));
  msg1 = _mm_sha256msg2_epu32(msg1, msg0);
  msg3 = _mm_sha256msg1_epu32(msg3, msg0);
  // [36, 39]
  msg = _mm_add_epi32(msg1, _mm_load_si128(K_mm + 9));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg2 = _mm_add_epi32(msg2, _mm_alignr_epi8(msg1, msg0, 4));
  msg2 = _mm_sha256msg2_epu32(msg2, msg1);
  msg0 = _mm_sha256msg1_epu32(msg0, msg1);
  // [40, 43]
  msg = _mm_add_epi32(msg2, _mm_load_si128(K_mm + 10));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg3 = _mm_add_epi32(msg3, _mm_alignr_epi8(msg2, msg1, 4));
  msg3 = _mm_sha256msg2_epu32(msg3, msg2);
  msg1 = _mm_sha256msg1_epu32(msg1, msg2);
  // [44, 47]
  msg = _mm_add_epi32(msg3, _mm_load_si128(K_mm + 11));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg0 = _mm_add_epi32(msg0, _mm_alignr_epi8(msg3, msg2, 4));
  msg0 = _mm_sha256msg2_epu32(msg0, msg3);
  msg2 = _mm_sha256msg1_epu32(msg2, msg3);
  // [48, 51]
  msg = _mm_add_epi32(msg0, _mm_load_si128(K_mm + 12));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg1 = _mm_add_epi32(msg1, _mm_alignr_epi8(msg0, msg3, 4));
  msg1 = _mm_sha256msg2_epu32(msg1, msg0);
  msg3 = _mm_sha256msg1_epu32(msg3, msg0);
  // [52, 55]
  msg = _mm_add_epi32(msg1, _mm_load_si128(K_mm + 13));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg2 = _mm_add_epi32(msg2, _mm_alignr_epi8(msg1, msg0, 4));
  msg2 = _mm_sha256msg2_epu32(msg2, msg1);
  // [56, 59]
  msg = _mm_add_epi32(msg2, _mm_load_si128(K_mm + 14));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));
  msg3 = _mm_add_epi32(msg3, _mm_alignr_epi8(msg2, msg1, 4));
  msg3 = _mm_sha256msg2_epu32(msg3, msg2);
  // [60, 63]
  msg = _mm_add_epi32(msg3, _mm_load_si128(K_mm + 15));
  state[1] = _mm_sha256rnds2_epu32(state[1], state[0], msg);
  state[0] = _mm_sha256rnds2_epu32(state[0], state[1], _mm_shuffle_epi32(msg, 0x0E));

  state[0] = _mm_add_epi32(state[0], old0);
  state[1] = _mm_add_epi32(state[1], old1);
}

static inline void sha256FinalSHANI(const __m128i state[2], uint32_t state32[8])
{
  __m128i state0 = _mm_shuffle_epi32(state[0], 0x1B); // FEBA
  __m128i state1 = _mm_shuffle_epi32(state[1], 0xB1); // DCHG

  // Save state
  _mm_storeu_si128(reinterpret_cast<__m128i*>(&state32[0]), _mm_blend_epi16(state0, state1, 0xF0)); // DCBA
  _mm_storeu_si128(reinterpret_cast<__m128i*>(&state32[4]), _mm_alignr_epi8(state1, state0, 8)); // ABEF
//  // TODO: use simd
//  for (unsigned i = 0; i < 8; i++)
//    state32[i] = __builtin_bswap32(state32[i]);
}
