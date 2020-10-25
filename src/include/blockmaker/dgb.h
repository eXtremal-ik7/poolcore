#pragma once

#include "btc.h"

namespace DGB {
class Proto {
public:
  static constexpr const char *TickerName = "DGB";

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
  static bool checkConsensus(const DGB::Proto::BlockHeader&, CheckConsensusCtx&, DGB::Proto::ChainParams&, double *shareDiff);
  static bool checkConsensus(const DGB::Proto::Block &block, CheckConsensusCtx &ctx, DGB::Proto::ChainParams &chainParams, double *shareDiff) { return checkConsensus(block.header, ctx, chainParams, shareDiff); }
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
      DGB::Proto::CheckConsensusCtx ctx;
      DGB::Proto::ChainParams params;
      DGB::Proto::checkConsensusInitialize(ctx);
      return DGB::Proto::checkConsensus(Header, ctx, params, shareDiff);
    }
  };

  static inline bool suitableForProfitSwitcher(const std::string&) { return true; }
  static inline double getIncomingProfitValue(rapidjson::Value &document, double price, double coeff) { return BTC::Stratum::getIncomingProfitValue(document, price, coeff); }
};

struct X {
  using Proto = DGB::Proto;
  using Stratum = DGB::Stratum;
  template<typename T> static inline void serialize(xmstream &src, const T &data) { BTC::Io<T>::serialize(src, data); }
  template<typename T> static inline void unserialize(xmstream &dst, T &data) { BTC::Io<T>::unserialize(dst, data); }
};
}
