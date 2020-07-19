#pragma once

#include <stdint.h>
#include <string.h>
#include <string>
#include "blockmaker/serialize.h"
#include "poolcommon/uint256.h"
#include <openssl/sha.h>
#include "rapidjson/document.h"
#include "loguru.hpp"

namespace BTC {
namespace Script {
  enum {
    OP_0 = 0,
    OP_RETURN = 0x6A,
    OP_DUP = 0x76,
    OP_EQUALVERIFY = 0x88,
    OP_HASH160 = 0xA9,
    OP_CHECKSIG = 0xAC
  };
}

class Proto {
public:
  static constexpr const char *TickerName = "BTC";

  using BlockHashTy = ::uint256;
  using TxHashTy = ::uint256;
  using AddressTy = ::uint160;

#pragma pack(push, 1)
  struct BlockHeader {
    int32_t nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;

    BlockHashTy GetHash() const {
      uint256 result;
      SHA256_CTX sha256;
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, this, sizeof(*this));
      SHA256_Final(result.begin(), &sha256);

      SHA256_Init(&sha256);
      SHA256_Update(&sha256, result.begin(), sizeof(result));
      SHA256_Final(result.begin(), &sha256);
      return result;
    }
  };
#pragma pack(pop)

  struct TxIn {
    uint256 previousOutputHash;
    uint32_t previousOutputIndex;
    xvector<uint8_t> scriptSig;
    xvector<xvector<uint8_t>> witnessStack;
    uint32_t sequence;

    static bool unpackWitnessStack(xmstream &src, DynamicPtr<BTC::Proto::TxIn> dst) {
      BTC::unpack(src, DynamicPtr<decltype (dst->witnessStack)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxIn, witnessStack)));
      return !dst->witnessStack.empty();
    }

    void serialize(xmstream &stream) const;
    void unserialize(xmstream &stream);
    static void unpack(xmstream &stream, xmstream &out, size_t offset);
    static void unpackFinalize(xmstream &out, size_t offset);
  };

  struct TxOut {
    int64_t value;
    xvector<uint8_t> pkScript;

    void serialize(xmstream &stream) const;
    void unserialize(xmstream &stream);
    static void unpack(xmstream &stream, xmstream &out, size_t offset);
    static void unpackFinalize(xmstream &out, size_t offset);
  };

  struct TxWitness {
    std::vector<uint8_t> data;
  };

  template<typename T>
  struct BlockHeaderNetTy {
    typename T::BlockHeader header;
  };

  template<typename T>
  struct BlockTy {
    typename T::BlockHeader header;
    xvector<typename T::Transaction> vtx;
  };

  template<typename T>
  struct TransactionTy {
    int32_t version;
    xvector<typename T::TxIn> txIn;
    xvector<typename T::TxOut> txOut;
    uint32_t lockTime;

    // Memory only
    uint32_t SerializedDataOffset = 0;
    uint32_t SerializedDataSize = 0;
    BTC::Proto::TxHashTy Hash;

    bool hasWitness() const {
      for (size_t i = 0; i < txIn.size(); i++) {
        if (!txIn[i].witnessStack.empty())
          return true;
      }

      return false;
    }

    BlockHashTy GetHash() const {
      if (!Hash.IsNull())
         return Hash;

      uint256 result;
      uint8_t buffer[4096];
      xmstream stream(buffer, sizeof(buffer));
      stream.reset();
      BTC::serialize(stream, *this);

      SHA256_CTX sha256;
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, stream.data(), stream.sizeOf());
      SHA256_Final(result.begin(), &sha256);
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, result.begin(), sizeof(result));
      SHA256_Final(result.begin(), &sha256);
      Hash = result;
      return result;
    }
  };

  using Block = BlockTy<BTC::Proto>;
  using Transaction = TransactionTy<BTC::Proto>;
  using MessageBlock = Block;

  static bool loadHeaderFromTemplate(BTC::Proto::BlockHeader &header, rapidjson::Value &blockTemplate);
};
}

namespace BTC {

// Header
template<> struct Io<Proto::BlockHeader> {
  static void serialize(xmstream &dst, const BTC::Proto::BlockHeader &data);
  static void unserialize(xmstream &src, BTC::Proto::BlockHeader &data);
  static void unpack(xmstream &src, DynamicPtr<BTC::Proto::BlockHeader> dst) { unserialize(src, *dst.ptr()); }
  static void unpackFinalize(DynamicPtr<BTC::Proto::BlockHeader>) {}
};

// TxIn
template<> struct Io<Proto::TxIn> {
  static void serialize(xmstream &dst, const BTC::Proto::TxIn &data);
  static void unserialize(xmstream &src, BTC::Proto::TxIn &data);
  static void unpack(xmstream &src, DynamicPtr<BTC::Proto::TxIn> dst);
  static void unpackFinalize(DynamicPtr<BTC::Proto::TxIn> dst);
};

// TxOut
template<> struct Io<Proto::TxOut> {
  static void serialize(xmstream &dst, const BTC::Proto::TxOut &data);
  static void unserialize(xmstream &src, BTC::Proto::TxOut &data);
  static void unpack(xmstream &src, DynamicPtr<BTC::Proto::TxOut> dst);
  static void unpackFinalize(DynamicPtr<BTC::Proto::TxOut> dst);
};

// Transaction
template<typename T> struct Io<Proto::TransactionTy<T>> {
  static inline void serialize(xmstream &dst, const BTC::Proto::TransactionTy<T> &data) {
    uint8_t flags = 0;
    BTC::serialize(dst, data.version);
    if (data.hasWitness()) {
      flags = 1;
      BTC::serializeVarSize(dst, 0);
      BTC::serialize(dst, flags);
    }

    BTC::serialize(dst, data.txIn);
    BTC::serialize(dst, data.txOut);

    if (flags) {
      for (size_t i = 0; i < data.txIn.size(); i++)
        BTC::serialize(dst, data.txIn[i].witnessStack);
    }

    BTC::serialize(dst, data.lockTime);
  }

  static inline void unserialize(xmstream &src, BTC::Proto::TransactionTy<T> &data) {
    uint8_t flags = 0;
    BTC::unserialize(src, data.version);
    BTC::unserialize(src, data.txIn);
    if (data.txIn.empty()) {
      BTC::unserialize(src, flags);
      if (flags != 0) {
        BTC::unserialize(src, data.txIn);
        BTC::unserialize(src, data.txOut);
      }
    } else {
      BTC::unserialize(src, data.txOut);
    }

    if (flags & 1) {
      flags ^= 1;

      for (size_t i = 0; i < data.txIn.size(); i++)
        BTC::unserialize(src, data.txIn[i].witnessStack);

      if (!data.hasWitness()) {
        src.seekEnd(0, true);
        return;
      }
    }

    if (flags) {
      src.seekEnd(0, true);
      return;
    }

    BTC::unserialize(src, data.lockTime);
  }

  static inline void unpack(xmstream &src, DynamicPtr<BTC::Proto::TransactionTy<T>> dst) {
    uint8_t flags = 0;

    BTC::unserialize(src, dst->version);
    BTC::unpack(src, DynamicPtr<decltype(dst->txIn)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::TransactionTy<T>, txIn)));

    if (dst->txIn.empty()) {
      BTC::unserialize(src, flags);
      if (flags) {
        BTC::unpack(src, DynamicPtr<decltype(dst->txIn)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::TransactionTy<T>, txIn)));
        BTC::unpack(src, DynamicPtr<decltype(dst->txOut)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::TransactionTy<T>, txOut)));
      }
    } else {
      BTC::unpack(src, DynamicPtr<decltype(dst->txOut)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::TransactionTy<T>, txOut)));
    }

    if (flags & 1) {
      flags ^= 1;

      bool hasWitness = false;
      for (size_t i = 0, ie = dst->txIn.size(); i < ie; i++) {
        size_t txInDataOffset = reinterpret_cast<size_t>(dst->txIn.data());
        size_t txInOffset = txInDataOffset + sizeof(typename T::TxIn)*i;
        hasWitness |= BTC::Proto::TxIn::unpackWitnessStack(src, DynamicPtr<Proto::TxIn>(dst.stream(), txInOffset));
      }

      if (!hasWitness) {
        src.seekEnd(0, true);
        return;
      }
    }

    if (flags) {
      src.seekEnd(0, true);
      return;
    }

    BTC::unserialize(src, dst->lockTime);
  }

  static inline void unpackFinalize(DynamicPtr<BTC::Proto::TransactionTy<T>> dst) {
    BTC::unpackFinalize(DynamicPtr<decltype(dst->txIn)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::TransactionTy<T>, txIn)));
    BTC::unpackFinalize(DynamicPtr<decltype(dst->txOut)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::TransactionTy<T>, txOut)));
  }
};

// Block
template<typename T> struct Io<Proto::BlockTy<T>> {
  static inline void serialize(xmstream &dst, const BTC::Proto::BlockTy<T> &data) {
    BTC::serialize(dst, data.header);
    BTC::serialize(dst, data.vtx);
  }

  static inline void unserialize(xmstream &src, BTC::Proto::BlockTy<T> &data) {
    size_t blockDataOffset = src.offsetOf();

    BTC::unserialize(src, data.header);

    uint64_t txNum = 0;
    unserializeVarSize(src, txNum);
    if (txNum > src.remaining()) {
      src.seekEnd(0, true);
      return;
    }

    data.vtx.resize(txNum);
    for (uint64_t i = 0; i < txNum; i++) {
      data.vtx[i].SerializedDataOffset = static_cast<uint32_t>(src.offsetOf() - blockDataOffset);
      BTC::unserialize(src, data.vtx[i]);
      data.vtx[i].SerializedDataSize = static_cast<uint32_t>(src.offsetOf() - data.vtx[i].SerializedDataOffset);
    }
  }

  static inline void unpack(xmstream &src, DynamicPtr<BTC::Proto::BlockTy<T>> dst) {
    size_t blockDataOffset = src.offsetOf();
    BTC::unpack(src, DynamicPtr<decltype (dst->header)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, header)));

    {
      auto vtxDst = DynamicPtr<decltype (dst->vtx)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, vtx));
      uint64_t txNum;
      unserializeVarSize(src, txNum);
      size_t dataOffset = vtxDst.stream().offsetOf();
      vtxDst.stream().reserve(txNum*sizeof(typename T::Transaction));

      new (vtxDst.ptr()) xvector<typename T::Transaction>(reinterpret_cast<typename T::Transaction*>(dataOffset), txNum, false);
      for (uint64_t i = 0; i < txNum; i++) {
        auto txPtr = DynamicPtr<typename T::Transaction>(vtxDst.stream(), dataOffset + sizeof(typename T::Transaction)*i);
        txPtr->SerializedDataOffset = static_cast<uint32_t>(src.offsetOf() - blockDataOffset);
        BTC::unpack(src, txPtr);
        txPtr->SerializedDataSize = static_cast<uint32_t>(src.offsetOf() - txPtr->SerializedDataOffset);
      }
    }
  }

  static inline void unpackFinalize(DynamicPtr<BTC::Proto::BlockTy<T>> dst) {
    BTC::unpackFinalize(DynamicPtr<decltype (dst->header)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, header)));
    BTC::unpackFinalize(DynamicPtr<decltype (dst->vtx)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, vtx)));
  }
};
}


bool loadTransactionsFromTemplate(xvector<BTC::Proto::Transaction> &vtx, rapidjson::Value &blockTemplate, xmstream &buffer);
bool buildSegwitCoinbaseFromTemplate(BTC::Proto::Transaction &coinbaseTx, BTC::Proto::AddressTy &address, rapidjson::Value &blockTemplate);
bool buildCoinbaseFromTemplate(BTC::Proto::Transaction &coinbaseTx, BTC::Proto::AddressTy &address, const std::string &coinbaseMessage, rapidjson::Value &blockTemplate, size_t *extraNonceOffset);
bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, BTC::Proto::AddressTy &address);
