#include "blockmaker/dgb.h"
#include "blockmaker/sph_skein.h"
#include "blockmaker/sph_luffa.h"
#include "blockmaker/sph_cubehash.h"
#include "blockmaker/sph_shavite.h"
#include "blockmaker/sph_simd.h"
#include "blockmaker/sph_echo.h"
#include "blockmaker/odocrypt.h"
#include "blockmaker/sha3.h"
#include "poolcommon/uint.h"

static uint32_t OdoKey(uint32_t nOdoShapechangeInterval, uint32_t nTime)
{
  uint32_t nShapechangeInterval = nOdoShapechangeInterval;
  return nTime - nTime % nShapechangeInterval;
}

namespace DGB {

template<> CCheckStatus Proto<DGB::Algo::EQubit>::checkConsensus(const Proto<DGB::Algo::EQubit>::BlockHeader &header, CheckConsensusCtx&, DGB::Proto<DGB::Algo::EQubit>::ChainParams&)
{
  CCheckStatus status;
  UInt<256> result;
  sph_luffa512_context	 ctx_luffa;
  sph_cubehash512_context  ctx_cubehash;
  sph_shavite512_context	 ctx_shavite;
  sph_simd512_context		 ctx_simd;
  sph_echo512_context		 ctx_echo;
  BaseBlob<512> hash[5];

  sph_luffa512_init(&ctx_luffa);
  sph_luffa512(&ctx_luffa, &header, sizeof(Proto<DGB::Algo::ESkein>::BlockHeader));
  sph_luffa512_close(&ctx_luffa, &hash[0]);

  sph_cubehash512_init(&ctx_cubehash);
  sph_cubehash512 (&ctx_cubehash, static_cast<const void*>(&hash[0]), 64);
  sph_cubehash512_close(&ctx_cubehash, static_cast<void*>(&hash[1]));

  sph_shavite512_init(&ctx_shavite);
  sph_shavite512(&ctx_shavite, static_cast<const void*>(&hash[1]), 64);
  sph_shavite512_close(&ctx_shavite, static_cast<void*>(&hash[2]));

  sph_simd512_init(&ctx_simd);
  sph_simd512 (&ctx_simd, static_cast<const void*>(&hash[2]), 64);
  sph_simd512_close(&ctx_simd, static_cast<void*>(&hash[3]));

  sph_echo512_init(&ctx_echo);
  sph_echo512 (&ctx_echo, static_cast<const void*>(&hash[3]), 64);
  sph_echo512_close(&ctx_echo, static_cast<void*>(&hash[4]));

  memcpy(result.rawData(), hash[4].begin(), 32);
  for (unsigned i = 0; i < 4; i++)
    result.data()[i] = readle(result.data()[i]);

  status.ShareDiff = BTC::difficultyFromBits(uint256GetCompact(result), 29);

  bool fNegative;
  bool fOverflow;
  UInt<256> bnTarget = uint256Compact(header.nBits, &fNegative, &fOverflow);

  // Check range
  if (fNegative || bnTarget == 0 || fOverflow)
    return status;

  // Check proof of work matches claimed amount
  if (result > bnTarget)
    return status;

  status.IsBlock = true;
  return status;
}

template<> CCheckStatus Proto<DGB::Algo::ESkein>::checkConsensus(const Proto<DGB::Algo::ESkein>::BlockHeader &header, CheckConsensusCtx&, DGB::Proto<DGB::Algo::ESkein>::ChainParams&)
{
  CCheckStatus status;
  CCtxSha256 sha256Context;
  sph_skein512_context skeinContext;
  BaseBlob<512> skeinHash;
  UInt<256> result;

  sph_skein512_init(&skeinContext);
  sph_skein512(&skeinContext, &header, sizeof(Proto<DGB::Algo::ESkein>::BlockHeader));
  sph_skein512_close(&skeinContext, &skeinHash);

  sha256Init(&sha256Context);
  sha256Update(&sha256Context, skeinHash.begin(), skeinHash.size());
  sha256Final(&sha256Context, result.rawData());
  for (unsigned i = 0; i < 4; i++)
    result.data()[i] = readle(result.data()[i]);

  status.ShareDiff = BTC::difficultyFromBits(uint256GetCompact(result), 29);

  bool fNegative;
  bool fOverflow;
  UInt<256> bnTarget = uint256Compact(header.nBits, &fNegative, &fOverflow);

  // Check range
  if (fNegative || bnTarget == 0 || fOverflow)
    return status;

  // Check proof of work matches claimed amount
  if (result > bnTarget)
    return status;

  status.IsBlock = true;
  return status;
}

template<> CCheckStatus Proto<DGB::Algo::EOdo>::checkConsensus(const Proto<DGB::Algo::EOdo>::BlockHeader &header, CheckConsensusCtx &ctx, DGB::Proto<DGB::Algo::EOdo>::ChainParams&)
{
  CCheckStatus status;
  uint32_t key = OdoKey(ctx.OdoShapechangeInterval, header.nTime);

  char cipher[100] = {};
  UInt<256> result;

  size_t len = sizeof(Proto<DGB::Algo::EOdo>::BlockHeader);
  memcpy(cipher, &header, len);
  cipher[len] = 1;

  OdoCrypt(key).Encrypt(cipher, cipher);
  for (unsigned i = 22-12; i < 22; i++)
    sha3llRound800((uint32_t*)cipher, i);
  memcpy(result.rawData(), cipher, result.rawSize());
  for (unsigned i = 0; i < 4; i++)
    result.data()[i] = readle(result.data()[i]);

  status.ShareDiff = BTC::difficultyFromBits(uint256GetCompact(result), 29);

  bool fNegative;
  bool fOverflow;
  UInt<256> bnTarget = uint256Compact(header.nBits, &fNegative, &fOverflow);

  // Check range
  if (fNegative || bnTarget == 0 || fOverflow)
    return status;

  // Check proof of work matches claimed amount
  if (result > bnTarget)
    return status;

  status.IsBlock = true;
  return status;
}

}
