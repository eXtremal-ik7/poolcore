// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "btc.h"

namespace LTC {
class Proto {
public:
  static constexpr const char *TickerName = "LTC";

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

  static bool checkPow(const Proto::BlockHeader &header, uint32_t nBits, double *shareDiff);
  static void checkConsensusInitialize(CheckConsensusCtx&) {}
  static bool checkConsensus(const LTC::Proto::BlockHeader &header, CheckConsensusCtx&, LTC::Proto::ChainParams&, double *shareDiff) { return checkPow(header, header.nBits, shareDiff); }
  static bool checkConsensus(const LTC::Proto::Block &block, CheckConsensusCtx&, LTC::Proto::ChainParams&, double *shareDiff) { return checkPow(block.header, block.header.nBits, shareDiff); }
};

class Stratum {
public:
  using MiningConfig = BTC::Stratum::MiningConfig;
  using WorkerConfig = BTC::Stratum::WorkerConfig;
  using ThreadConfig = BTC::Stratum::ThreadConfig;
  static constexpr double DifficultyFactor = 65536.0;

  class Work : public BTC::Stratum::Work {
  public:
    bool checkConsensus(size_t, double *shareDiff) {
      LTC::Proto::CheckConsensusCtx ctx;
      LTC::Proto::ChainParams params;
      LTC::Proto::checkConsensusInitialize(ctx);
      return LTC::Proto::checkConsensus(Header, ctx, params, shareDiff);
    }
  };

  static inline bool suitableForProfitSwitcher(const std::string&) { return true; }
  static inline double getIncomingProfitValue(rapidjson::Value &document, double price, double coeff) { return BTC::Stratum::getIncomingProfitValue(document, price, coeff); }
};

struct X {
  using Proto = LTC::Proto;
  using Stratum = LTC::Stratum;
  template<typename T> static inline void serialize(xmstream &src, const T &data) { BTC::Io<T>::serialize(src, data); }
  template<typename T> static inline void unserialize(xmstream &dst, T &data) { BTC::Io<T>::unserialize(dst, data); }
};
}
