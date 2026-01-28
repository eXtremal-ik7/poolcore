// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "btc.h"
#include "serialize.h"
#include "blockmaker/x11.h"

namespace DASH {

class Proto {
public:
    static constexpr const char* TickerName = "DASH";

    using BlockHashTy = BTC::Proto::BlockHashTy;
    using TxHashTy    = BTC::Proto::TxHashTy;
    using AddressTy   = BTC::Proto::AddressTy;
    using BlockHeader = BTC::Proto::BlockHeader;
    using Block       = BTC::Proto::Block;
    using TxIn        = BTC::Proto::TxIn;
    using TxOut       = BTC::Proto::TxOut;

    struct Transaction {
        int32_t           version;
        xvector<TxIn>     txIn;
        xvector<TxOut>    txOut;
        uint32_t          lockTime;
        xvector<uint8_t>  vExtraPayload;  // only for special TX types

        // Memory-only cache fields
        uint32_t          SerializedDataOffset = 0;
        uint32_t          SerializedDataSize   = 0;
        TxHashTy          Hash;

        bool hasWitness() const { return false; }
    };

    using CheckConsensusCtx = BTC::Proto::CheckConsensusCtx;
    using ChainParams       = BTC::Proto::ChainParams;

    static CCheckStatus checkPow(const BlockHeader &header, uint32_t nBits, const UInt<256> &shareTarget);
    static void checkConsensusInitialize(CheckConsensusCtx&) {}
    static CCheckStatus checkConsensus(const BlockHeader &header, CheckConsensusCtx&, ChainParams&, const UInt<256> &shareTarget) {
        return checkPow(header, header.nBits, shareTarget);
    }
    static CCheckStatus checkConsensus(const Block &block, CheckConsensusCtx&, ChainParams&, const UInt<256> &shareTarget) {
        return checkPow(block.header, block.header.nBits, shareTarget);
    }
    static double getDifficulty(const BlockHeader &header) {
        return BTC::difficultyFromBits(header.nBits, 29);
    }
    static UInt<256> expectedWork(const BlockHeader &header, const CheckConsensusCtx&) {
      return uint256Compact(header.nBits);
    }
    static bool decodeHumanReadableAddress(const std::string &hrAddress,
                                           const std::vector<uint8_t> &prefix,
                                           AddressTy &address) {
        return BTC::Proto::decodeHumanReadableAddress(hrAddress, prefix, address);
    }
};

} // namespace DASH

namespace BTC {

// Serialize/unserialize DASH transactions
template<>
struct Io<DASH::Proto::Transaction> {
    static void serialize(xmstream &dst, const DASH::Proto::Transaction &data, bool /*serializeWitness*/ = false) {
        BTC::serialize(dst, data.version);
        BTC::serialize(dst, data.txIn);
        BTC::serialize(dst, data.txOut);
        BTC::serialize(dst, data.lockTime);
        BTC::serialize(dst, data.vExtraPayload);
    }

    static void unserialize(xmstream &src, DASH::Proto::Transaction &data) {
        BTC::unserialize(src, data.version);
        BTC::unserialize(src, data.txIn);
        BTC::unserialize(src, data.txOut);
        BTC::unserialize(src, data.lockTime);
        BTC::unserialize(src, data.vExtraPayload);
    }

    static void unpack(xmstream &src, DynamicPtr<DASH::Proto::Transaction> dst) {
        unserialize(src, *dst.ptr());
    }

    static void unpackFinalize(DynamicPtr<DASH::Proto::Transaction>) {}
};

} // namespace BTC

// Stratum support for Dash (X11)
namespace DASH {

class Stratum {
public:
    inline static const UInt<256> StratumMultiplier = UInt<256>(1u) << 16;
    using Work = BTC::WorkTy<DASH::Proto,
                             BTC::Stratum::HeaderBuilder,
                             BTC::Stratum::CoinbaseBuilder,
                             BTC::Stratum::Notify,
                             BTC::Stratum::Prepare>;
    static constexpr bool MergedMiningSupport = false;

    static Work* newPrimaryWork(int64_t stratumId,
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

    static StratumSingleWork* newSecondaryWork(int64_t, PoolBackend*, size_t,
                                               const CMiningConfig&, const std::vector<uint8_t>&,
                                               const std::string&, CBlockTemplate&, const std::string&) {
        return nullptr;
    }

    static StratumMergedWork* newMergedWork(int64_t, StratumSingleWork*,
                                            std::vector<StratumSingleWork*>&,
                                            const CMiningConfig&, std::string&) {
        return nullptr;
    }

    static EStratumDecodeStatusTy decodeStratumMessage(CStratumMessage &msg,
                                                        const char *in,
                                                        size_t size) {
        return BTC::Stratum::decodeStratumMessage(msg, in, size);
    }

    static void miningConfigInitialize(CMiningConfig &miningCfg,
                                       rapidjson::Value &instanceCfg) {
        BTC::Stratum::miningConfigInitialize(miningCfg, instanceCfg);
    }

    static void workerConfigInitialize(CWorkerConfig &workerCfg,
                                       ThreadConfig &threadCfg) {
        BTC::Stratum::workerConfigInitialize(workerCfg, threadCfg);
    }

    static void workerConfigSetupVersionRolling(CWorkerConfig &workerCfg,
                                                uint32_t versionMask) {
        BTC::Stratum::workerConfigSetupVersionRolling(workerCfg, versionMask);
    }

    static void workerConfigOnSubscribe(CWorkerConfig &workerCfg,
                                        CMiningConfig &miningCfg,
                                        CStratumMessage &msg,
                                        xmstream &out,
                                        std::string &subscribeInfo) {
        BTC::Stratum::workerConfigOnSubscribe(workerCfg,
                                              miningCfg,
                                              msg,
                                              out,
                                              subscribeInfo);
    }

    static void buildSendTargetMessage(xmstream &stream, double difficulty) {
        BTC::Stratum::buildSendTargetMessageImpl(stream, difficulty);
    }
    static UInt<256> targetFromDifficulty(const UInt<256> &difficulty) { return BTC::Stratum::targetFromDifficulty(difficulty); }
};

struct X {
    using Proto   = DASH::Proto;
    using Stratum = DASH::Stratum;

    template<typename T>
    static inline void serialize(xmstream &dst, const T &data) {
        BTC::Io<T>::serialize(dst, data);
    }

    template<typename T>
    static inline void unserialize(xmstream &src, T &data) {
        BTC::Io<T>::unserialize(src, data);
    }
};

} // namespace DASH
