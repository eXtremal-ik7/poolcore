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
  static bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, AddressTy &address) { return BTC::Proto::decodeHumanReadableAddress(hrAddress, pubkeyAddressPrefix, address); }
};

class Stratum {
public:
  static constexpr double DifficultyFactor = 65536.0;

  using MiningConfig = BTC::Stratum::MiningConfig;
  using WorkerConfig = BTC::Stratum::WorkerConfig;
  using StratumMessage = BTC::Stratum::StratumMessage;
  using Work = BTC::WorkTy<DGB::Proto, BTC::Stratum::HeaderBuilder, BTC::Stratum::CoinbaseBuilder, BTC::Stratum::Notify, BTC::Stratum::Prepare, MiningConfig, WorkerConfig, StratumMessage>;
  using SecondWork = StratumSingleWorkEmpty<Proto::BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>;
  using MergedWork = StratumMergedWorkEmpty<Proto::BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>;

  static constexpr bool MergedMiningSupport = false;
  static bool isMainBackend(const std::string&) { return true; }
  static bool keepOldWorkForBackend(const std::string&) { return false; }
  static void buildSendTargetMessage(xmstream &stream, double difficulty) { BTC::Stratum::buildSendTargetMessageImpl(stream, difficulty, DifficultyFactor); }
};

struct X {
  using Proto = DGB::Proto;
  using Stratum = DGB::Stratum;
  template<typename T> static inline void serialize(xmstream &src, const T &data) { BTC::Io<T>::serialize(src, data); }
  template<typename T> static inline void unserialize(xmstream &dst, T &data) { BTC::Io<T>::unserialize(dst, data); }
};
}
