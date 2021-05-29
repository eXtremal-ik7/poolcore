#include "blockmaker/doge.h"
#include "blockmaker/merkleTree.h"
#include "blockmaker/serializeJson.h"

static const unsigned char pchMergedMiningHeader[] = { 0xfa, 0xbe, 'm', 'm' };

static uint32_t getExpectedIndex(uint32_t nNonce, int nChainId, unsigned h)
{
  uint32_t rand = nNonce;
  rand = rand * 1103515245 + 12345;
  rand += nChainId;
  rand = rand * 1103515245 + 12345;

  return rand % (1 << h);
}

namespace DOGE {

Stratum::MergedWork::MergedWork(uint64_t stratumWorkId, StratumSingleWork *first, StratumSingleWork *second, MiningConfig &miningCfg) : StratumMergedWork(stratumWorkId, first, second, miningCfg)
{
  LTCHeader_ = ltcWork()->Header;
  LTCMerklePath_ = ltcWork()->MerklePath;
  DOGEHeader_ = dogeWork()->Header;

  // Prepare merged work
  // <Merged mining signature> <chain merkle root> <chain merkle tree size> <extra nonce fixed part>
  // Really:
  //   <0xfa, 0xbe, 'm', 'm'> <doge header hash> <0x01, 0x00, 0x00, 0x00> <0x00, 0x00, 0x00, 0x00>

  // We need DOGE header hash, but we have not merkle root now
  // For calculate merkle root, we need non-mutable DOGE coinbase transaction (without extra nonce) and merkle path (already available)

  // Create 'static' DOGE coinbase transaction without extra nonce
  MiningConfig emptyExtraNonceConfig;
  emptyExtraNonceConfig.FixedExtraNonceSize = 0;
  emptyExtraNonceConfig.MutableExtraNonceSize = 0;
  dogeWork()->buildCoinbaseTx(nullptr, 0, emptyExtraNonceConfig, DOGELegacy_, DOGEWitness_);

  // Calculate merkle root
  DOGEHeader_.nVersion |= DOGE::Proto::BlockHeader::VERSION_AUXPOW;
  DOGEHeader_.hashMerkleRoot = calculateMerkleRoot(DOGELegacy_.Data.data(), DOGELegacy_.Data.sizeOf(), dogeWork()->MerklePath);

  // Calculate /reversed/ DOGE header hash
  uint256 hash = DOGEHeader_.GetHash();
  std::reverse(hash.begin(), hash.end());

  // Prepare LTC coinbase
  uint8_t buffer[1024];
  xmstream coinbaseMsg(buffer, sizeof(buffer));
  coinbaseMsg.reset();
  coinbaseMsg.write(pchMergedMiningHeader, sizeof(pchMergedMiningHeader));
  coinbaseMsg.write(hash.begin(), sizeof(uint256));
  coinbaseMsg.write<uint32_t>(1);
  coinbaseMsg.write<uint32_t>(0);
  ltcWork()->buildCoinbaseTx(coinbaseMsg.data(), coinbaseMsg.sizeOf(), miningCfg, LTCLegacy_, LTCWitness_);
}

bool Stratum::MergedWork::prepareForSubmit(const CWorkerConfig &workerCfg, const StratumMessage &msg)
{
  if (LTC::Stratum::Work::prepareForSubmitImpl(LTCHeader_, LTCHeader_.nVersion, LTCLegacy_, LTCWitness_, LTCMerklePath_, workerCfg, MiningCfg_, msg))
    return false;

  uint32_t chainId = DOGEHeader_.nVersion >> 16;
  LTCWitness_.Data.seekSet(0);
  BTC::unserialize(LTCWitness_.Data, DOGEHeader_.ParentBlockCoinbaseTx);

  DOGEHeader_.HashBlock.SetNull();
  DOGEHeader_.MerkleBranch.resize(LTCMerklePath_.size());
  for (size_t i = 0, ie = LTCMerklePath_.size(); i != ie; ++i)
    DOGEHeader_.MerkleBranch[i] = LTCMerklePath_[i];

  DOGEHeader_.Index = 0;
  DOGEHeader_.ChainMerkleBranch.resize(0);
  DOGEHeader_.ChainIndex = getExpectedIndex(0, chainId, 0);
  DOGEHeader_.ParentBlock = LTCHeader_;
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
