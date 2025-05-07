#include "blockmaker/x11.h"

#include "blockmaker/sph_blake.h"
#include "blockmaker/sph_bmw.h"
#include "blockmaker/sph_groestl.h"
#include "blockmaker/sph_jh.h"
#include "blockmaker/sha3.h"
#include "blockmaker/sph_skein.h"
#include "blockmaker/sph_luffa.h"
#include "blockmaker/sph_cubehash.h"
#include "blockmaker/sph_shavite.h"
#include "blockmaker/sph_simd.h"
#include "blockmaker/sph_echo.h"

#include <string.h>

void x11_hash(const uint8_t* input, uint32_t len, uint8_t* output) {
    static const uint8_t pblank[1] = { 0 };

    sph_blake512_context     ctx_blake;
    sph_bmw512_context       ctx_bmw;
    sph_groestl512_context   ctx_groestl;
    sph_jh512_context        ctx_jh;
    sph_skein512_context     ctx_skein;
    sph_luffa512_context     ctx_luffa;
    sph_cubehash512_context  ctx_cubehash;
    sph_shavite512_context   ctx_shavite;
    sph_simd512_context      ctx_simd;
    sph_echo512_context      ctx_echo;

    uint8_t hash[11][64];

    const void* data = (input == NULL || len == 0) ? pblank : input;
    size_t datalen = (input == NULL || len == 0) ? 1 : len;

    sph_blake512_init(&ctx_blake);
    sph_blake512(&ctx_blake, data, datalen);
    sph_blake512_close(&ctx_blake, &hash[0]);

    sph_bmw512_init(&ctx_bmw);
    sph_bmw512(&ctx_bmw, &hash[0], 64);
    sph_bmw512_close(&ctx_bmw, &hash[1]);

    sph_groestl512_init(&ctx_groestl);
    sph_groestl512(&ctx_groestl, &hash[1], 64);
    sph_groestl512_close(&ctx_groestl, &hash[2]);

    sph_skein512_init(&ctx_skein);
    sph_skein512(&ctx_skein, &hash[2], 64);
    sph_skein512_close(&ctx_skein, &hash[3]);

    sph_jh512_init(&ctx_jh);
    sph_jh512(&ctx_jh, &hash[3], 64);
    sph_jh512_close(&ctx_jh, &hash[4]);

    {
      CCtxSha3 ctx;
      sha3Init(&ctx, 64);
      sha3Update(&ctx, &hash[4], 64);
      sha3Final(&ctx, (uint8_t*)&hash[5], 1);
    }

    sph_luffa512_init(&ctx_luffa);
    sph_luffa512(&ctx_luffa, &hash[5], 64);
    sph_luffa512_close(&ctx_luffa, &hash[6]);

    sph_cubehash512_init(&ctx_cubehash);
    sph_cubehash512(&ctx_cubehash, &hash[6], 64);
    sph_cubehash512_close(&ctx_cubehash, &hash[7]);

    sph_shavite512_init(&ctx_shavite);
    sph_shavite512(&ctx_shavite, &hash[7], 64);
    sph_shavite512_close(&ctx_shavite, &hash[8]);

    sph_simd512_init(&ctx_simd);
    sph_simd512(&ctx_simd, &hash[8], 64);
    sph_simd512_close(&ctx_simd, &hash[9]);

    sph_echo512_init(&ctx_echo);
    sph_echo512(&ctx_echo, &hash[9], 64);
    sph_echo512_close(&ctx_echo, &hash[10]);

    memcpy(output, &hash[10], 32);
}
