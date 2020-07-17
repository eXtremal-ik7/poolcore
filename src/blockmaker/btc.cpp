// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockmaker/btc.h"
#include "poolcore/base58.h"

namespace BTC {

void Io<Proto::BlockHeader>::serialize(xmstream &dst, const BTC::Proto::BlockHeader &data)
{
  BTC::serialize(dst, data.nVersion);
  BTC::serialize(dst, data.hashPrevBlock);
  BTC::serialize(dst, data.hashMerkleRoot);
  BTC::serialize(dst, data.nTime);
  BTC::serialize(dst, data.nBits);
  BTC::serialize(dst, data.nNonce);
}

void Io<Proto::BlockHeader>::unserialize(xmstream &src, BTC::Proto::BlockHeader &data)
{
  BTC::unserialize(src, data.nVersion);
  BTC::unserialize(src, data.hashPrevBlock);
  BTC::unserialize(src, data.hashMerkleRoot);
  BTC::unserialize(src, data.nTime);
  BTC::unserialize(src, data.nBits);
  BTC::unserialize(src, data.nNonce);
}

void Io<Proto::TxIn>::serialize(xmstream &stream, const BTC::Proto::TxIn &data)
{
  BTC::serialize(stream, data.previousOutputHash);
  BTC::serialize(stream, data.previousOutputIndex);
  BTC::serialize(stream, data.scriptSig);
  BTC::serialize(stream, data.sequence);
}

void Io<Proto::TxIn>::unserialize(xmstream &stream, BTC::Proto::TxIn &data)
{
  BTC::unserialize(stream, data.previousOutputHash);
  BTC::unserialize(stream, data.previousOutputIndex);
  BTC::unserialize(stream, data.scriptSig);
  BTC::unserialize(stream, data.sequence);
}

void Io<Proto::TxIn>::unpack(xmstream &src, DynamicPtr<BTC::Proto::TxIn> dst)
{
  {
    BTC::Proto::TxIn *ptr = dst.ptr();
    BTC::unserialize(src, ptr->previousOutputHash);
    BTC::unserialize(src, ptr->previousOutputIndex);
  }

  BTC::unpack(src, DynamicPtr<decltype (dst->scriptSig)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxIn, scriptSig)));
  {
    BTC::Proto::TxIn *ptr = dst.ptr();
    new (&ptr->witnessStack) decltype(ptr->witnessStack)();
    BTC::unserialize(src, ptr->sequence);
  }
}

void Io<Proto::TxIn>::unpackFinalize(DynamicPtr<BTC::Proto::TxIn> dst)
{
  BTC::unpackFinalize(DynamicPtr<decltype (dst->scriptSig)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxIn, scriptSig)));
  BTC::unpackFinalize(DynamicPtr<decltype (dst->witnessStack)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxIn, witnessStack)));
}

void Io<Proto::TxOut>::serialize(xmstream &src, const BTC::Proto::TxOut &data)
{
  BTC::serialize(src, data.value);
  BTC::serialize(src, data.pkScript);
}

void Io<Proto::TxOut>::unserialize(xmstream &dst, BTC::Proto::TxOut &data)
{
  BTC::unserialize(dst, data.value);
  BTC::unserialize(dst, data.pkScript);
}

void Io<Proto::TxOut>::unpack(xmstream &src, DynamicPtr<BTC::Proto::TxOut> dst)
{
  BTC::unserialize(src, dst->value);
  BTC::unpack(src, DynamicPtr<decltype (dst->pkScript)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxOut, pkScript)));
}

void Io<Proto::TxOut>::unpackFinalize(DynamicPtr<BTC::Proto::TxOut> dst)
{
  BTC::unpackFinalize(DynamicPtr<decltype (dst->pkScript)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxOut, pkScript)));
}

}
