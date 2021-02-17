#pragma once

#include "ltc.h"

namespace DOGE {
class Proto {
public:
  static constexpr const char *TickerName = "DOGE";

  using BlockHashTy = BTC::Proto::BlockHashTy;
  using TxHashTy = BTC::Proto::TxHashTy;
  using AddressTy = BTC::Proto::AddressTy;

  using PureBlockHeader = LTC::Proto::BlockHeader;

  using TxIn = BTC::Proto::TxIn;
  using TxOut = BTC::Proto::TxOut;
  using TxWitness = BTC::Proto::TxWitness;
  using Transaction = BTC::Proto::Transaction;

  struct BlockHeader: public PureBlockHeader {
  public:
    static const int32_t VERSION_AUXPOW = (1 << 8);
    // AuxPow
    Transaction ParentBlockCoinbaseTx;
    uint256 HashBlock;
    xvector<uint256> MerkleBranch;
    int Index;
    xvector<uint256> ChainMerkleBranch;
    int ChainIndex;
    PureBlockHeader ParentBlock;
  };

  using Block = BTC::Proto::BlockTy<DOGE::Proto>;

  using CheckConsensusCtx = BTC::Proto::CheckConsensusCtx;
  using ChainParams = BTC::Proto::ChainParams;

  static void checkConsensusInitialize(CheckConsensusCtx&) {}
  static bool checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx&, Proto::ChainParams&, double *shareDiff) {
    return header.nVersion & Proto::BlockHeader::VERSION_AUXPOW ?
      LTC::Proto::checkPow(header.ParentBlock, header.nBits, shareDiff) :
      LTC::Proto::checkPow(header, header.nBits, shareDiff);
  }

  static bool checkConsensus(const Proto::Block &block, CheckConsensusCtx &ctx, Proto::ChainParams &chainParams, double *shareDiff) { return checkConsensus(block.header, ctx, chainParams, shareDiff); }
};

class Stratum {
public:
  using MiningConfig = BTC::Stratum::MiningConfig;
  using WorkerConfig = BTC::Stratum::WorkerConfig;
  using ThreadConfig = BTC::Stratum::ThreadConfig;
  static constexpr double DifficultyFactor = 65536.0;

  class Work {
  public:
    size_t backendsNum() { return 2; }
    // TODO: check (LTC/DOGE hash differ)
    uint256 shareHash() { return LTC.Header.GetHash(); }
    uint256 blockHash(size_t index) { return index == 0 ? DOGE.Header.GetHash() : LTC.Header.GetHash(); }

    PoolBackend *backend(size_t index) { return index == 0 ? DOGE.Backend : LTC.Backend; }
    uint64_t height(size_t index) { return index == 0 ? DOGE.Height : LTC.Height; }
    size_t txNum(size_t index) { return index == 0 ? DOGE.TxNum : LTC.TxNum; }
    int64_t blockReward(size_t index) { return index == 0 ? DOGE.BlockReward : LTC.BlockReward; }
    double expectedWork(size_t index);/* { return index == 0 ? DOGE.expectedWork(0) : LTC.expectedWork(0); }*/
    const xmstream &blockHexData(size_t index) { return index == 0 ? DOGE.PreparedBlockData : LTC.BlockHexData; }
    bool initialized() { return LTC.Backend != nullptr; }
    bool isNewBlock(const PoolBackend *backend, const std::string &ticker, uint64_t workId) {
      if (ticker == "DOGE" || ticker == "DOGE.testnet" || ticker == "DOGE.regtest")
        return false;

      return LTC.isNewBlock(backend, ticker, workId);
    }
    const xmstream &notifyMessage() { return LTC.notifyMessage(); }
    void mutate() { LTC.mutate(); }

    bool checkConsensus(size_t index, double *shareDiff) {
      if (index == 0) {
        Proto::CheckConsensusCtx ctx;
        Proto::ChainParams params;
        Proto::checkConsensusInitialize(ctx);
        return Proto::checkConsensus(DOGE.Header, ctx, params, shareDiff);
      } else {
        return LTC.checkConsensus(0, shareDiff);
      }
    }

    void buildNotifyMessage(MiningConfig &cfg, uint64_t majorJobId, unsigned minorJobId, bool resetPreviousWork) {
      LTC.buildNotifyMessage(cfg, majorJobId, minorJobId, resetPreviousWork);
    }

    void updateLTCBlock();
    void updateDOGEBlock();
    bool loadFromTemplate(rapidjson::Value &document,
                          uint64_t uniqueWorkId,
                          const MiningConfig &cfg,
                          PoolBackend *backend,
                          const std::string &ticker,
                          Proto::AddressTy &miningAddress,
                          const void *coinBaseExtraData,
                          size_t coinbaseExtraSize,
                          std::string &error,
                          uint32_t txNumLimit=0);

    bool prepareForSubmit(const WorkerConfig &workerCfg, const MiningConfig &miningCfg, const StratumMessage &msg);

  private:
    LTC::Stratum::Work LTC;

    struct {
      uint64_t UniqueWorkId;
      Proto::BlockHeader Header;
      PoolBackend *Backend = nullptr;
      uint64_t Height = 0;
      int64_t BlockReward;
      xmstream TxData;
      xmstream PreparedBlockData;
      size_t TxNum;
    } DOGE;

  };

  static bool suitableForProfitSwitcher(const std::string &ticker) {
    if (ticker == "DOGE" || ticker == "DOGE.testnet" || ticker == "DOGE.regtest")
      return false;

    return LTC::Stratum::suitableForProfitSwitcher(ticker);
  }

  static double getIncomingProfitValue(rapidjson::Value &document, double price, double coeff) { return BTC::Stratum::getIncomingProfitValue(document, price, coeff); }
};

struct X {
  using Proto = DOGE::Proto;
  using Stratum = DOGE::Stratum;
  template<typename T> static inline void serialize(xmstream &src, const T &data) { BTC::Io<T>::serialize(src, data); }
  template<typename T> static inline void unserialize(xmstream &dst, T &data) { BTC::Io<T>::unserialize(dst, data); }
};
}

// Header
namespace BTC {
template<> struct Io<DOGE::Proto::BlockHeader> {
  static void serialize(xmstream &dst, const DOGE::Proto::BlockHeader &data);
  static void unserialize(xmstream &src, DOGE::Proto::BlockHeader &data);
};
}

void serializeJsonInside(xmstream &stream, const DOGE::Proto::BlockHeader &header);
