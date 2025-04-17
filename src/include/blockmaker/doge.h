#pragma once

#include "ltc.h"
#include "poolinstances/stratumWorkStorage.h"

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
  static CCheckStatus checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx&, Proto::ChainParams&) {
    return header.nVersion & Proto::BlockHeader::VERSION_AUXPOW ?
      LTC::Proto::checkPow(header.ParentBlock, header.nBits) :
      LTC::Proto::checkPow(header, header.nBits);
  }

  static CCheckStatus checkConsensus(const Proto::Block &block, CheckConsensusCtx &ctx, Proto::ChainParams &chainParams) { return checkConsensus(block.header, ctx, chainParams); }
  static double getDifficulty(const Proto::BlockHeader &header) { return BTC::difficultyFromBits(header.nBits, 29); }
  static double expectedWork(const Proto::BlockHeader &header, const CheckConsensusCtx&) { return getDifficulty(header); }
  static bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, AddressTy &address) { return BTC::Proto::decodeHumanReadableAddress(hrAddress, pubkeyAddressPrefix, address); }
};

class Stratum {
public:
  static constexpr double DifficultyFactor = 65536.0;
  using DogeWork = BTC::WorkTy<DOGE::Proto, BTC::Stratum::HeaderBuilder, BTC::Stratum::CoinbaseBuilder, BTC::Stratum::Notify, BTC::Stratum::Prepare>;

  class MergedWork : public StratumMergedWork {
  public:
    MergedWork(uint64_t stratumWorkId, StratumSingleWork *first, std::vector<StratumSingleWork*> &second, const CMiningConfig &miningCfg);

    virtual Proto::BlockHashTy shareHash() override {
      return LTCHeader_.GetHash();
    }

    virtual std::string blockHash(size_t workIdx) override {
      if (workIdx == 0)
        return LTCHeader_.GetHash().ToString();
      else if (workIdx - 1 < DOGEHeader_.size())
        return DOGEHeader_[workIdx-1].GetHash().ToString();
      else
        return std::string();
    }

    virtual void mutate() override {
      LTCHeader_.nTime = static_cast<uint32_t>(time(nullptr));
      LTC::Stratum::Work::buildNotifyMessageImpl(this, LTCHeader_, LTCHeader_.nVersion, LTCLegacy_, LTCMerklePath_, MiningCfg_, true, NotifyMessage_);
    }

    virtual void buildNotifyMessage(bool resetPreviousWork) override {
      LTC::Stratum::Work::buildNotifyMessageImpl(this, LTCHeader_, LTCHeader_.nVersion, LTCLegacy_, LTCMerklePath_, MiningCfg_, resetPreviousWork, NotifyMessage_);
    }

    virtual bool prepareForSubmit(const CWorkerConfig &workerCfg, const CStratumMessage &msg) override;

    virtual void buildBlock(size_t workIdx, xmstream &blockHexData) override {
      if (workIdx == 0 && ltcWork()) {
        ltcWork()->buildBlockImpl(LTCHeader_, LTCWitness_, blockHexData);
      } else if (dogeWork(workIdx - 1)) {
        dogeWork(workIdx - 1)->buildBlockImpl(DOGEHeader_[workIdx-1], DOGEWitness_[workIdx-1], blockHexData);
      }
    }

    virtual CCheckStatus checkConsensus(size_t workIdx) override {
      if (workIdx == 0 && ltcWork())
        return LTC::Stratum::Work::checkConsensusImpl(LTCHeader_, DOGEConsensusCtx_);
      else if (dogeWork(workIdx - 1))
        return DOGE::Stratum::DogeWork::checkConsensusImpl(DOGEHeader_[workIdx - 1], LTCConsensusCtx_);
      return CCheckStatus();
    }

  private:
    LTC::Stratum::Work *ltcWork() { return static_cast<LTC::Stratum::Work*>(Works_[0].Work); }
    DOGE::Stratum::DogeWork *dogeWork(unsigned index) { return static_cast<DOGE::Stratum::DogeWork*>(Works_[index + 1].Work); }

  private:
    LTC::Proto::BlockHeader LTCHeader_;
    BTC::CoinbaseTx LTCLegacy_;
    BTC::CoinbaseTx LTCWitness_;
    std::vector<uint256> LTCMerklePath_;
    LTC::Proto::CheckConsensusCtx LTCConsensusCtx_;

    std::vector<DOGE::Proto::BlockHeader> DOGEHeader_;
    std::vector<BTC::CoinbaseTx> DOGELegacy_;
    std::vector<BTC::CoinbaseTx> DOGEWitness_;
    std::vector<uint256> DOGEHeaderHashes_;
    DOGE::Proto::CheckConsensusCtx DOGEConsensusCtx_;
  };

  static constexpr bool MergedMiningSupport = true;
  static EStratumDecodeStatusTy decodeStratumMessage(CStratumMessage &msg, const char *in, size_t size) { return BTC::Stratum::decodeStratumMessage(msg, in, size); }
  static void miningConfigInitialize(CMiningConfig &miningCfg, rapidjson::Value &instanceCfg) { BTC::Stratum::miningConfigInitialize(miningCfg, instanceCfg); }
  static void workerConfigInitialize(CWorkerConfig &workerCfg, ThreadConfig &threadCfg) { BTC::Stratum::workerConfigInitialize(workerCfg, threadCfg); }
  static void workerConfigSetupVersionRolling(CWorkerConfig &workerCfg, uint32_t versionMask) { BTC::Stratum::workerConfigSetupVersionRolling(workerCfg, versionMask); }
  static void workerConfigOnSubscribe(CWorkerConfig &workerCfg, CMiningConfig &miningCfg, CStratumMessage &msg, xmstream &out, std::string &subscribeInfo) {
    BTC::Stratum::workerConfigOnSubscribe(workerCfg, miningCfg, msg, out, subscribeInfo);
  }

  static LTC::Stratum::Work *newPrimaryWork(int64_t stratumId,
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
    std::unique_ptr<LTC::Stratum::Work> work(new LTC::Stratum::Work(stratumId,
                                        blockTemplate.UniqueWorkId,
                                        backend,
                                        backendIdx,
                                        miningCfg,
                                        miningAddress,
                                        coinbaseMessage));
    return work->loadFromTemplate(blockTemplate, error) ? work.release() : nullptr;
  }
  static DogeWork *newSecondaryWork(int64_t stratumId,
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
    std::unique_ptr<DogeWork> work(new DogeWork(stratumId,
                                                blockTemplate.UniqueWorkId,
                                                backend,
                                                backendIdx,
                                                miningCfg,
                                                miningAddress,
                                                coinbaseMessage));
    return work->loadFromTemplate(blockTemplate, error) ? work.release() : nullptr;
  }
  static StratumMergedWork *newMergedWork(int64_t stratumId,
                                          StratumSingleWork *primaryWork,
                                          std::vector<StratumSingleWork*> &secondaryWorks,
                                          const CMiningConfig &miningCfg,
                                          std::string &error) {
    if (secondaryWorks.empty()) {
      error = "no secondary works";
      return nullptr;
    }
    return new MergedWork(stratumId, primaryWork, secondaryWorks, miningCfg);
  }

  static void buildSendTargetMessage(xmstream &stream, double difficulty) { BTC::Stratum::buildSendTargetMessageImpl(stream, difficulty, DifficultyFactor); }
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
