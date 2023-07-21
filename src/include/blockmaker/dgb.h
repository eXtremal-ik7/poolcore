#pragma once

#include "btc.h"

namespace DGB {
enum class Algo {
  EQubit,
  ESkein,
  EOdo
};

template<Algo algo>
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

  struct CheckConsensusCtx {
    void initialize(const std::string &ticker) {
      if (ticker.find(".testnet")) {
        OdoShapechangeInterval = 1*24*60*60; // 1 day
      } else if (ticker.find(".regtest") != ticker.npos) {
        OdoShapechangeInterval = 4; // 1 minute
      } else {
        // mainnet
        OdoShapechangeInterval = 10*24*60*60; // 10 days
      }
    }
    uint32_t OdoShapechangeInterval;
  };

  using ChainParams = BTC::Proto::ChainParams;

  static void checkConsensusInitialize(CheckConsensusCtx&) {}
  static bool checkConsensus(const BlockHeader&, CheckConsensusCtx&, ChainParams&, double *shareDiff);
  static bool checkConsensus(const Block &block, CheckConsensusCtx &ctx, ChainParams &chainParams, double *shareDiff) { return checkConsensus(block.header, ctx, chainParams, shareDiff); }
  static double getDifficulty(const Proto::BlockHeader &header) { return BTC::difficultyFromBits(header.nBits, 29); }
  static bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, AddressTy &address) { return BTC::Proto::decodeHumanReadableAddress(hrAddress, pubkeyAddressPrefix, address); }
};

template<Algo algo>
class Stratum {
private:
  static constexpr double factor() {
    switch(algo) {
      case Algo::EQubit : return 1.0;
      case Algo::ESkein : return 1.0;
      case Algo::EOdo : return 1.0;
    }
  }

public:
  static constexpr double DifficultyFactor = factor();

  using MiningConfig = BTC::Stratum::MiningConfig;
  using WorkerConfig = BTC::Stratum::WorkerConfig;
  using StratumMessage = BTC::Stratum::StratumMessage;
  using Work = BTC::WorkTy<Proto<algo>, BTC::Stratum::HeaderBuilder, BTC::Stratum::CoinbaseBuilder, BTC::Stratum::Notify, BTC::Stratum::Prepare, MiningConfig, WorkerConfig, StratumMessage>;
  using SecondWork = StratumSingleWorkEmpty<typename Proto<algo>::BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>;
  using MergedWork = StratumMergedWorkEmpty<typename Proto<algo>::BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>;

  static constexpr bool MergedMiningSupport = false;
  static bool isMainBackend(const std::string&) { return true; }
  static bool keepOldWorkForBackend(const std::string&) { return false; }
  static void buildSendTargetMessage(xmstream &stream, double difficulty) { BTC::Stratum::buildSendTargetMessageImpl(stream, difficulty, DifficultyFactor); }
};

template<Algo algo>
struct X {
  using Proto = DGB::Proto<algo>;
  using Stratum = DGB::Stratum<algo>;
  template<typename T> static inline void serialize(xmstream &src, const T &data) { BTC::Io<T>::serialize(src, data); }
  template<typename T> static inline void unserialize(xmstream &dst, T &data) { BTC::Io<T>::unserialize(dst, data); }
};
}
