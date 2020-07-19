// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockmaker/xpm.h"

namespace BTC {

void Io<mpz_class>::serialize(xmstream &dst, const mpz_class &data)
{
  constexpr mp_limb_t one = 1;
  auto bnSize = mpz_sizeinbase(data.get_mpz_t(), 256);
  auto serializedSize = bnSize;
  if (data.get_mpz_t()->_mp_size) {
      mp_limb_t signBit = one << (8*(bnSize % sizeof(mp_limb_t)) - 1);
      if (data.get_mpz_t()->_mp_d[data.get_mpz_t()->_mp_size-1] & signBit)
          serializedSize++;
  }

  serializeVarSize(dst, serializedSize);
  mpz_export(dst.reserve<uint8_t>(bnSize), nullptr, -1, 1, -1, 0, data.get_mpz_t());
  if (serializedSize > bnSize)
    dst.write<uint8_t>(0);
}

void Io<mpz_class>::unserialize(xmstream &src, mpz_class &data)
{
  uint64_t size;
  unserializeVarSize(src, size);
  if (const uint8_t *p = src.seek<uint8_t>(size))
    mpz_import(data.get_mpz_t(), size, -1, 1, -1, 0, p);
}

void Io<mpz_class>::unpack(xmstream &src, DynamicPtr<mpz_class> dst)
{
  uint64_t size;
  unserializeVarSize(src, size);
  if (const uint8_t *p = src.seek<uint8_t>(size)) {
    size_t alignedSize = size % sizeof(mp_limb_t) ? size + (sizeof(mp_limb_t) - size % sizeof(mp_limb_t)) : size;
    size_t limbsNum = alignedSize / sizeof(mp_limb_t);
    size_t dataOffset = dst.stream().offsetOf();

    dst.stream().reserve<mp_limb_t>(limbsNum);

    mpz_class *mpz = dst.ptr();
    mp_limb_t *data = reinterpret_cast<mp_limb_t*>(dst.stream().data<uint8_t>() + dataOffset);
    mpz->get_mpz_t()->_mp_d = data;
    mpz->get_mpz_t()->_mp_size = static_cast<int>(limbsNum);
    mpz->get_mpz_t()->_mp_alloc = static_cast<int>(limbsNum);
    mpz_import(mpz->get_mpz_t(), size, -1, 1, -1, 0, p);

    // Change address to stream offset
    mpz->get_mpz_t()->_mp_d = reinterpret_cast<mp_limb_t*>(dataOffset);
  }
}

void Io<mpz_class>::unpackFinalize(DynamicPtr<mpz_class> dst)
{
  mpz_class *mpz = dst.ptr();
  mpz->get_mpz_t()->_mp_d = reinterpret_cast<mp_limb_t*>(dst.stream().data<uint8_t>() + reinterpret_cast<size_t>(mpz->get_mpz_t()->_mp_d));
}

void Io<XPM::Proto::BlockHeader>::serialize(xmstream &dst, const XPM::Proto::BlockHeader &data)
{
  BTC::serialize(dst, data.nVersion);
  BTC::serialize(dst, data.hashPrevBlock);
  BTC::serialize(dst, data.hashMerkleRoot);
  BTC::serialize(dst, data.nTime);
  BTC::serialize(dst, data.nBits);
  BTC::serialize(dst, data.nNonce);
  BTC::serialize(dst, data.bnPrimeChainMultiplier);
}

void Io<XPM::Proto::BlockHeader>::unserialize(xmstream &src, XPM::Proto::BlockHeader &data)
{
  BTC::unserialize(src, data.nVersion);
  BTC::unserialize(src, data.hashPrevBlock);
  BTC::unserialize(src, data.hashMerkleRoot);
  BTC::unserialize(src, data.nTime);
  BTC::unserialize(src, data.nBits);
  BTC::unserialize(src, data.nNonce);
  BTC::unserialize(src, data.bnPrimeChainMultiplier);
}

void Io<XPM::Proto::BlockHeader>::unpack(xmstream &src, DynamicPtr<XPM::Proto::BlockHeader> dst)
{
  XPM::Proto::BlockHeader *ptr = dst.ptr();

  // TODO: memcpy on little-endian
  BTC::unserialize(src, ptr->nVersion);
  BTC::unserialize(src, ptr->hashPrevBlock);
  BTC::unserialize(src, ptr->hashMerkleRoot);
  BTC::unserialize(src, ptr->nTime);
  BTC::unserialize(src, ptr->nBits);
  BTC::unserialize(src, ptr->nNonce);

  // unserialize prime chain multiplier
  BTC::unpack(src, DynamicPtr<mpz_class>(dst.stream(), dst.offset()+offsetof(XPM::Proto::BlockHeader, bnPrimeChainMultiplier)));
}

void Io<XPM::Proto::BlockHeader>::unpackFinalize(DynamicPtr<XPM::Proto::BlockHeader> dst)
{
  BTC::unpackFinalize(DynamicPtr<mpz_class>(dst.stream(), dst.offset()+offsetof(XPM::Proto::BlockHeader, bnPrimeChainMultiplier)));
}
}

namespace XPM {
bool Proto::loadHeaderFromTemplate(XPM::Proto::BlockHeader &header, rapidjson::Value &blockTemplate)
{
  // version
  if (!blockTemplate.HasMember("version") || !blockTemplate["version"].IsUint())
    return false;
  header.nVersion = blockTemplate["version"].GetUint();

  // hashPrevBlock
  if (!blockTemplate.HasMember("previousblockhash") || !blockTemplate["previousblockhash"].IsString())
    return false;
  header.hashPrevBlock.SetHex(blockTemplate["previousblockhash"].GetString());

  // hashMerkleRoot
  header.hashMerkleRoot.SetNull();

  // time
  if (!blockTemplate.HasMember("mintime") || !blockTemplate["mintime"].IsUint())
    return false;
  header.nTime = blockTemplate["mintime"].GetUint();

  // bits
  if (!blockTemplate.HasMember("bits") || !blockTemplate["bits"].IsString())
    return false;
  header.nBits = strtoul(blockTemplate["bits"].GetString(), nullptr, 16);

  // nonce
  header.nNonce = 0;

  // bnPrimeChainMultiplier
  header.bnPrimeChainMultiplier = 0;
  return true;
}
}
