// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "btc.h"

namespace DASH {

enum {
  TRANSACTION_NORMAL = 0
  // Additional Dash transaction types can be added here
};

struct Transaction {
  int16_t nVersion;
  uint16_t nType;
  xvector<TxIn> vin;
  xvector<TxOut> vout;
  uint32_t nLockTime;
  xvector<uint8_t> vExtraPayload;

  template<typename Stream>
  void serialize(Stream &s) const {
    int32_t n32bitVersion = (nType << 16) | (uint16_t)nVersion;
    s << n32bitVersion;
    s << vin;
    s << vout;
    s << nLockTime;
    if (nVersion >= 3 && nType != TRANSACTION_NORMAL)
      s << vExtraPayload;
  }

  template<typename Stream>
  void unserialize(Stream &s) {
    int32_t n32bitVersion;
    s >> n32bitVersion;
    nVersion = (int16_t)(n32bitVersion & 0xffff);
    nType = (uint16_t)((n32bitVersion >> 16) & 0xffff);
    s >> vin;
    s >> vout;
    s >> nLockTime;
    if (nVersion >= 3 && nType != TRANSACTION_NORMAL)
      s >> vExtraPayload;
  }
};

class Proto {
public:
  static constexpr const char *TickerName = "DASH";

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

  static CCheckStatus checkPow(const Proto::BlockHeader &header, uint32_t nBits);
  static void checkConsensusInitialize(CheckConsensusCtx&) {}
  static CCheckStatus checkConsensus(const DASH::Proto::BlockHeader &header, CheckConsensusCtx&, DASH::Proto::ChainParams&) { return checkPow(header, header.nBits); }
  static CCheckStatus checkConsensus(const DASH::Proto::Block &block, CheckConsensusCtx&, DASH::Proto::ChainParams&) { return checkPow(block.header, block.header.nBits); }
  static double getDifficulty(const Proto::BlockHeader &header) { return BTC::difficultyFromBits(header.nBits, 29); }
  static double expectedWork(const Proto::BlockHeader &header, const CheckConsensusCtx&) { return getDifficulty(header); }
  static bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, AddressTy &address) { return BTC::Proto::decodeHumanReadableAddress(hrAddress, pubkeyAddressPrefix, address); }
};

class Stratum {
public:
  static constexpr double DifficultyFactor = 65536.0;

  using Work = BTC::WorkTy<DASH::Proto, BTC::Stratum::HeaderBuilder, BTC::Stratum::CoinbaseBuilder, BTC::Stratum::Notify, BTC::Stratum::Prepare>;
  static constexpr bool MergedMiningSupport = false;

  static Work *newPrimaryWork(int64_t stratumId,
      PoolBackend *backend,
      size_t backendIdx,
      const CMiningConfig &miningCfg,
      const std::vector<uint8_t> &miningAddress,
      const std::string &coinbaseMessage,
      CBlockTemplate &blockTemplate,
      std::string &error) {
    if (blockTemplate.WorkType != EWorkBitcoin) {
      error = "incompatible work type";
      return nullptr;
    }

    std::unique_ptr<Work> work(new Work(stratumId,
                                        blockTemplate.UniqueWorkId,
                                        backend,
                                        backendIdx,
                                        miningCfg,
                                        miningAddress,
                                        coinbaseMessage));
    return work->loadFromTemplate(blockTemplate, error) ? work.release() : nullptr;
  }
  static StratumSingleWork *newSecondaryWork(int64_t, PoolBackend*, size_t, const CMiningConfig&, const std::vector<uint8_t>&, const std::string&, CBlockTemplate&, const std::string&) { return nullptr; }
  static StratumMergedWork *newMergedWork(int64_t, StratumSingleWork*, std::vector<StratumSingleWork*>&, const CMiningConfig&, std::string&) { return nullptr; }

  static EStratumDecodeStatusTy decodeStratumMessage(CStratumMessage &msg, const char *in, size_t size) { return BTC::Stratum::decodeStratumMessage(msg, in, size); }
  static void miningConfigInitialize(CMiningConfig &miningCfg, rapidjson::Value &instanceCfg) { BTC::Stratum::miningConfigInitialize(miningCfg, instanceCfg); }
  static void workerConfigInitialize(CWorkerConfig &workerCfg, ThreadConfig &threadCfg) { BTC::Stratum::workerConfigInitialize(workerCfg, threadCfg); }
  static void workerConfigSetupVersionRolling(CWorkerConfig &workerCfg, uint32_t versionMask) { BTC::Stratum::workerConfigSetupVersionRolling(workerCfg, versionMask); }
  static void workerConfigOnSubscribe(CWorkerConfig &workerCfg, CMiningConfig &miningCfg, CStratumMessage &msg, xmstream &out, std::string &subscribeInfo) {
    BTC::Stratum::workerConfigOnSubscribe(workerCfg, miningCfg, msg, out, subscribeInfo);
  }

  static void buildSendTargetMessage(xmstream &stream, double difficulty) { BTC::Stratum::buildSendTargetMessageImpl(stream, difficulty, DifficultyFactor); }
};

struct X {
  using Proto = DASH::Proto;
  using Stratum = DASH::Stratum;
  template<typename T> static inline void serialize(xmstream &src, const T &data) { BTC::Io<T>::serialize(src, data); }
  template<typename T> static inline void unserialize(xmstream &dst, T &data) { BTC::Io<T>::unserialize(dst, data); }
};
}
