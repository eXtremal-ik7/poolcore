#pragma once

#include "ltc.h"

namespace DOGE {
class Proto {
public:
  static constexpr const char *TickerName = "DOGE";

  using BlockHashTy = BTC::Proto::BlockHashTy;
  using TxHashTy = BTC::Proto::TxHashTy;
  using AddressTy = BTC::Proto::AddressTy;
  using BlockHeader = BTC::Proto::BlockHeader;
  using Block = BTC::Proto::Block;
  using TxIn = BTC::Proto::TxIn;
  using TxOut = BTC::Proto::TxOut;
  using TxWitness = BTC::Proto::TxWitness;
  using Transaction = BTC::Proto::Transaction;

  using CheckConsensusCtx = BTC::Proto::CheckConsensusCtx;
  using ChainParams = BTC::Proto::ChainParams;

  static void checkConsensusInitialize(CheckConsensusCtx&) {}
  static bool checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx &ctx, Proto::ChainParams &chainParams, double *shareDiff) { return LTC::Proto::checkConsensus(header, ctx, chainParams, shareDiff); }
  static bool checkConsensus(const Proto::Block &block, CheckConsensusCtx &ctx, Proto::ChainParams &chainParams, double *shareDiff) { return checkConsensus(block.header, ctx, chainParams, shareDiff); }
};

using Stratum = LTC::Stratum;

struct X {
  using Proto = DOGE::Proto;
  using Stratum = DOGE::Stratum;
  template<typename T> static inline void serialize(xmstream &src, const T &data) { BTC::Io<T>::serialize(src, data); }
  template<typename T> static inline void unserialize(xmstream &dst, T &data) { BTC::Io<T>::unserialize(dst, data); }
};
}
