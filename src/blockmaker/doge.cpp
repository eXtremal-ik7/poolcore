#include "blockmaker/doge.h"
#include "blockmaker/merkleTree.h"
#include "blockmaker/serializeJson.h"

static const unsigned char pchMergedMiningHeader[] = { 0xfa, 0xbe, 'm', 'm' };

static unsigned merklePathSize(unsigned count)
{
  return count > 1 ? (31 - __builtin_clz((count << 1) - 1)) : 0;
}

static uint32_t getExpectedIndex(uint32_t nNonce, int nChainId, unsigned h)
{
  uint32_t rand = nNonce;
  rand = rand * 1103515245 + 12345;
  rand += nChainId;
  rand = rand * 1103515245 + 12345;

  return rand % (1 << h);
}

namespace DOGE {

Stratum::MergedWork::MergedWork(uint64_t stratumWorkId, StratumSingleWork *first, std::vector<StratumSingleWork*> &second, const CMiningConfig &miningCfg) : StratumMergedWork(stratumWorkId, first, second, miningCfg)
{
  LTCHeader_ = ltcWork()->Header;
  LTCMerklePath_ = ltcWork()->MerklePath;
  LTCConsensusCtx_ = ltcWork()->ConsensusCtx_;

  DOGEHeader_.resize(second.size());
  DOGELegacy_.resize(second.size());
  DOGEWitness_.resize(second.size());
  DOGEHeaderHashes_.resize(second.size());
  DOGEWorkMap_.resize(second.size());

  uint32_t nonce = 0;
  unsigned pathSize = merklePathSize(second.size());
  for (;;) {
    std::fill(DOGEWorkMap_.begin(), DOGEWorkMap_.end(), -1);

    bool found = true;
    for (size_t workIdx = 0; workIdx < second.size(); workIdx++) {
      DOGE::Stratum::DogeWork *work = dogeWork(workIdx);
      unsigned index = getExpectedIndex(nonce, work->Header.nVersion >> 16, pathSize);
      if (DOGEWorkMap_[index] == -1) {
        DOGEWorkMap_[index] = workIdx;
      } else {
        found = false;
        break;
      }
    }

    if (found)
      break;

    nonce++;
  }

  for (size_t workIdx = 0; workIdx < DOGEHeader_.size(); workIdx++) {
    DOGE::Stratum::DogeWork *work = dogeWork(workIdx);
    DOGE::Proto::BlockHeader &header = DOGEHeader_[workIdx];
    BTC::CoinbaseTx &legacy = DOGELegacy_[workIdx];
    BTC::CoinbaseTx &witness = DOGEWitness_[workIdx];

    header = work->Header;

    // Prepare merged work
    // <Merged mining signature> <chain merkle root> <chain merkle tree size> <extra nonce fixed part>
    // Really:
    //   <0xfa, 0xbe, 'm', 'm'> <doge header hash> <0x01, 0x00, 0x00, 0x00> <0x00, 0x00, 0x00, 0x00>

    // We need DOGE header hash, but we have not merkle root now
    // For calculate merkle root, we need non-mutable DOGE coinbase transaction (without extra nonce) and merkle path (already available)

    // Create 'static' DOGE coinbase transaction without extra nonce
    CMiningConfig emptyExtraNonceConfig;
    emptyExtraNonceConfig.FixedExtraNonceSize = 0;
    emptyExtraNonceConfig.MutableExtraNonceSize = 0;
    work->buildCoinbaseTx(nullptr, 0, emptyExtraNonceConfig, legacy, witness);

    // Calculate merkle root
    header.nVersion |= DOGE::Proto::BlockHeader::VERSION_AUXPOW;
    {
      uint256 coinbaseTxHash;
      CCtxSha256 sha256;
      sha256Init(&sha256);
      sha256Update(&sha256, legacy.Data.data(), legacy.Data.sizeOf());
      sha256Final(&sha256, coinbaseTxHash.begin());
      sha256Init(&sha256);
      sha256Update(&sha256, coinbaseTxHash.begin(), coinbaseTxHash.size());
      sha256Final(&sha256, coinbaseTxHash.begin());
      header.hashMerkleRoot = calculateMerkleRootWithPath(coinbaseTxHash, &work->MerklePath[0], work->MerklePath.size(), 0);
    }

    DOGEHeaderHashes_[DOGEWorkMap_[workIdx]] = header.GetHash();
  }

  // Calculate /reversed/ merkle root from DOGE header hashes
  uint256 hash = calculateMerkleRoot(&DOGEHeaderHashes_[0], DOGEHeaderHashes_.size());
  std::reverse(hash.begin(), hash.end());

  // Prepare LTC coinbase
  uint8_t buffer[1024];
  xmstream coinbaseMsg(buffer, sizeof(buffer));
  coinbaseMsg.reset();
  coinbaseMsg.write(pchMergedMiningHeader, sizeof(pchMergedMiningHeader));
  coinbaseMsg.write(hash.begin(), sizeof(uint256));
  coinbaseMsg.write<uint32_t>(1u << pathSize);
  coinbaseMsg.write<uint32_t>(nonce);
  ltcWork()->buildCoinbaseTx(coinbaseMsg.data(), coinbaseMsg.sizeOf(), miningCfg, LTCLegacy_, LTCWitness_);

  DOGEConsensusCtx_ = dogeWork(0)->ConsensusCtx_;
}

bool Stratum::MergedWork::prepareForSubmit(const CWorkerConfig &workerCfg, const CStratumMessage &msg)
{
  if (!LTC::Stratum::Work::prepareForSubmitImpl(LTCHeader_, LTCHeader_.nVersion, LTCLegacy_, LTCWitness_, LTCMerklePath_, workerCfg, MiningCfg_, msg))
    return false;

  for (size_t workIdx = 0; workIdx < DOGEHeader_.size(); workIdx++) {
    DOGE::Proto::BlockHeader &header = DOGEHeader_[workIdx];
    LTCWitness_.Data.seekSet(0);
    BTC::unserialize(LTCWitness_.Data, header.ParentBlockCoinbaseTx);

    header.HashBlock.SetNull();
    header.Index = 0;

    header.MerkleBranch.resize(LTCMerklePath_.size());
    for (size_t j = 0, je = LTCMerklePath_.size(); j != je; ++j)
      header.MerkleBranch[j] = LTCMerklePath_[j];

    std::vector<uint256> path;
    buildMerklePath(DOGEHeaderHashes_, DOGEWorkMap_[workIdx], path);
    header.ChainMerkleBranch.resize(path.size());
    for (size_t j = 0; j < path.size(); j++)
      header.ChainMerkleBranch[j] = path[j];
    header.ChainIndex = DOGEWorkMap_[workIdx];
    header.ParentBlock = LTCHeader_;
  }

  return true;
}
}

void BTC::Io<DOGE::Proto::BlockHeader>::serialize(xmstream &dst, const DOGE::Proto::BlockHeader &data)
{
  BTC::serialize(dst, *(DOGE::Proto::PureBlockHeader*)&data);
  if (data.nVersion & DOGE::Proto::BlockHeader::VERSION_AUXPOW) {
    BTC::serialize(dst, data.ParentBlockCoinbaseTx);

    BTC::serialize(dst, data.HashBlock);
    BTC::serialize(dst, data.MerkleBranch);
    BTC::serialize(dst, data.Index);

    BTC::serialize(dst, data.ChainMerkleBranch);
    BTC::serialize(dst, data.ChainIndex);
    BTC::serialize(dst, data.ParentBlock);
  }
}

void serializeJsonInside(xmstream &stream, const DOGE::Proto::BlockHeader &header)
{
  serializeJson(stream, "version", header.nVersion); stream.write(',');
  serializeJson(stream, "hashPrevBlock", header.hashPrevBlock); stream.write(',');
  serializeJson(stream, "hashMerkleRoot", header.hashMerkleRoot); stream.write(',');
  serializeJson(stream, "time", header.nTime); stream.write(',');
  serializeJson(stream, "bits", header.nBits); stream.write(',');
  serializeJson(stream, "nonce", header.nNonce); stream.write(',');
  serializeJson(stream, "parentBlockCoinbaseTx", header.ParentBlockCoinbaseTx); stream.write(',');
  serializeJson(stream, "hashBlock", header.HashBlock); stream.write(',');
  serializeJson(stream, "merkleBranch", header.MerkleBranch); stream.write(',');
  serializeJson(stream, "index", header.Index); stream.write(',');
  serializeJson(stream, "chainMerkleBranch", header.ChainMerkleBranch); stream.write(',');
  serializeJson(stream, "chainIndex", header.ChainIndex); stream.write(',');
  stream.write("\"parentBlock\":{");
  serializeJsonInside(stream, header.ParentBlock);
  stream.write('}');
}
