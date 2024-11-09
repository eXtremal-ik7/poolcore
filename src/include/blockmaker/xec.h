// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "btc.h"

namespace XEC {
class Proto {
public:
  static constexpr const char *TickerName = "XEC";

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
  public:
    bool HasRtt = false;
    uint32_t PrevBits;
    int64_t PrevHeaderTime[4];

  public:
    void initialize(CBlockTemplate &blockTemplate, const std::string &ticker);
    bool hasRtt() { return HasRtt; }
  };

  using ChainParams = BTC::Proto::ChainParams;

  static CCheckStatus checkPow(const Proto::BlockHeader &header, uint32_t nBits);
  static void checkConsensusInitialize(CheckConsensusCtx&) {}
  static CCheckStatus checkConsensus(const XEC::Proto::BlockHeader &header, CheckConsensusCtx &ctx, XEC::Proto::ChainParams &params);
  static CCheckStatus checkConsensus(const XEC::Proto::Block &block, CheckConsensusCtx &ctx, XEC::Proto::ChainParams &params) { return checkConsensus(block.header, ctx, params); }
  static double getDifficulty(const Proto::BlockHeader &header) { return BTC::difficultyFromBits(header.nBits, 29); }
  static double expectedWork(const XEC::Proto::BlockHeader &header, const CheckConsensusCtx &ctx);
  static bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, AddressTy &address) { return BTC::Proto::decodeHumanReadableAddress(hrAddress, pubkeyAddressPrefix, address); }
};

class Stratum {
public:
  static constexpr double DifficultyFactor = 65536.0;

  using MiningConfig = BTC::Stratum::MiningConfig;
  using WorkerConfig = BTC::Stratum::WorkerConfig;
  using StratumMessage = BTC::Stratum::StratumMessage;

  using Work = BTC::WorkTy<XEC::Proto, BTC::Stratum::HeaderBuilder, BTC::Stratum::CoinbaseBuilder, BTC::Stratum::Notify, BTC::Stratum::Prepare, MiningConfig, WorkerConfig, StratumMessage>;
  using SecondWork = StratumSingleWorkEmpty<Proto::BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>;
  using MergedWork = StratumMergedWorkEmpty<Proto::BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>;
  static constexpr bool MergedMiningSupport = false;
  static constexpr bool HasRtt = true;
  static bool isMainBackend(const std::string&) { return true; }
  static bool keepOldWorkForBackend(const std::string&) { return false; }
  static void buildSendTargetMessage(xmstream &stream, double difficulty) { BTC::Stratum::buildSendTargetMessageImpl(stream, difficulty, DifficultyFactor); }
};

struct X {
  using Proto = XEC::Proto;
  using Stratum = XEC::Stratum;
  template<typename T> static inline void serialize(xmstream &src, const T &data) { BTC::Io<T>::serialize(src, data); }
  template<typename T> static inline void unserialize(xmstream &dst, T &data) { BTC::Io<T>::unserialize(dst, data); }
};
}
