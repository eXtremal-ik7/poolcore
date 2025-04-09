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
    void initialize(CBlockTemplate&, const std::string &ticker) {
      if (ticker.find(".testnet")) {
        OdoShapechangeInterval = 1*24*60*60; // 1 day
      } else if (ticker.find(".regtest") != ticker.npos) {
        OdoShapechangeInterval = 4; // 1 minute
      } else {
        // mainnet
        OdoShapechangeInterval = 10*24*60*60; // 10 days
      }
    }

    bool hasRtt() { return false; }

    uint32_t OdoShapechangeInterval;
  };

  using ChainParams = BTC::Proto::ChainParams;

  static void checkConsensusInitialize(CheckConsensusCtx&) {}
  static CCheckStatus checkConsensus(const BlockHeader&, CheckConsensusCtx&, ChainParams&);
  static CCheckStatus checkConsensus(const Block &block, CheckConsensusCtx &ctx, ChainParams &chainParams) { return checkConsensus(block.header, ctx, chainParams); }
  static double getDifficulty(const Proto::BlockHeader &header) { return BTC::difficultyFromBits(header.nBits, 29); }
  static double expectedWork(const Proto::BlockHeader &header, const CheckConsensusCtx&) { return getDifficulty(header); }
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

  using StratumMessage = BTC::Stratum::StratumMessage;
  using Work = BTC::WorkTy<Proto<algo>, BTC::Stratum::HeaderBuilder, BTC::Stratum::CoinbaseBuilder, BTC::Stratum::Notify, BTC::Stratum::Prepare, StratumMessage>;
  using SecondWork = StratumSingleWorkEmpty<typename Proto<algo>::BlockHashTy, StratumMessage>;
  using MergedWork = StratumMergedWorkEmpty<typename Proto<algo>::BlockHashTy, StratumMessage>;

  static constexpr bool MergedMiningSupport = false;
  static void miningConfigInitialize(CMiningConfig &miningCfg, rapidjson::Value &instanceCfg) { BTC::Stratum::miningConfigInitialize(miningCfg, instanceCfg); }
  static void workerConfigInitialize(CWorkerConfig &workerCfg, ThreadConfig &threadCfg) { BTC::Stratum::workerConfigInitialize(workerCfg, threadCfg); }
  static void workerConfigSetupVersionRolling(CWorkerConfig &workerCfg, uint32_t versionMask) { BTC::Stratum::workerConfigSetupVersionRolling(workerCfg, versionMask); }
  static void workerConfigOnSubscribe(CWorkerConfig &workerCfg, CMiningConfig &miningCfg, StratumMessage &msg, xmstream &out, std::string &subscribeInfo) {
    BTC::Stratum::workerConfigOnSubscribe(workerCfg, miningCfg, msg, out, subscribeInfo);
  }
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
