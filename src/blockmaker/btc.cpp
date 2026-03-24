// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockmaker/btc.h"
#include "blockmaker/merkleTree.h"
#include "blockmaker/serializeJson.h"
#include "poolcommon/uint.h"
#include "poolcore/base58.h"
#include "poolcommon/bech32.h"
#include <cmath>

namespace BTC {

void Io<Proto::BlockHeader>::serialize(xmstream &dst, const BTC::Proto::BlockHeader &data)

{
  BTC::serialize(dst, data.nVersion);
  BTC::serialize(dst, data.hashPrevBlock);
  BTC::serialize(dst, data.hashMerkleRoot);
  BTC::serialize(dst, data.nTime);
  BTC::serialize(dst, data.nBits);
  BTC::serialize(dst, data.nNonce);
}

void Io<Proto::BlockHeader>::unserialize(xmstream &src, BTC::Proto::BlockHeader &data)
{
  BTC::unserialize(src, data.nVersion);
  BTC::unserialize(src, data.hashPrevBlock);
  BTC::unserialize(src, data.hashMerkleRoot);
  BTC::unserialize(src, data.nTime);
  BTC::unserialize(src, data.nBits);
  BTC::unserialize(src, data.nNonce);
}

void Io<Proto::TxIn>::serialize(xmstream &stream, const BTC::Proto::TxIn &data)
{
  BTC::serialize(stream, data.previousOutputHash);
  BTC::serialize(stream, data.previousOutputIndex);
  BTC::serialize(stream, data.scriptSig);
  BTC::serialize(stream, data.sequence);
}

void Io<Proto::TxIn>::unserialize(xmstream &stream, BTC::Proto::TxIn &data)
{
  BTC::unserialize(stream, data.previousOutputHash);
  BTC::unserialize(stream, data.previousOutputIndex);
  BTC::unserialize(stream, data.scriptSig);
  BTC::unserialize(stream, data.sequence);
}

size_t Proto::TxIn::scriptSigOffset()
{
  size_t result = 0;
  result += sizeof(previousOutputHash);
  result += sizeof(previousOutputIndex);
  result += serializedVarSizeLength(scriptSig.size());
  return result;
}

void Io<Proto::TxOut>::serialize(xmstream &src, const BTC::Proto::TxOut &data)
{
  BTC::serialize(src, data.value);
  BTC::serialize(src, data.pkScript);
}

void Io<Proto::TxOut>::unserialize(xmstream &dst, BTC::Proto::TxOut &data)
{
  BTC::unserialize(dst, data.value);
  BTC::unserialize(dst, data.pkScript);
}

void Io<Proto::Transaction>::serialize(xmstream &dst, const BTC::Proto::Transaction &data, bool serializeWitness)
{
  uint8_t flags = 0;
  BTC::serialize(dst, data.version);
  if (data.hasWitness() && serializeWitness) {
    flags = 1;
    BTC::serializeVarSize(dst, 0);
    BTC::serialize(dst, flags);
  }

  BTC::serialize(dst, data.txIn);
  BTC::serialize(dst, data.txOut);

  if (flags) {
    for (size_t i = 0; i < data.txIn.size(); i++)
      BTC::serialize(dst, data.txIn[i].witnessStack);
  }

  BTC::serialize(dst, data.lockTime);
}

void Io<Proto::Transaction>::unserialize(xmstream &src, BTC::Proto::Transaction &data)
{
  uint8_t flags = 0;
  BTC::unserialize(src, data.version);
  BTC::unserialize(src, data.txIn);
  if (data.txIn.empty()) {
    BTC::unserialize(src, flags);
    if (flags != 0) {
      BTC::unserialize(src, data.txIn);
      BTC::unserialize(src, data.txOut);
    }
  } else {
    BTC::unserialize(src, data.txOut);
  }

  if (flags & 1) {
    flags ^= 1;

    for (size_t i = 0; i < data.txIn.size(); i++)
      BTC::unserialize(src, data.txIn[i].witnessStack);

    if (!data.hasWitness()) {
      src.seekEnd(0, true);
      return;
    }
  }

  if (flags) {
    src.seekEnd(0, true);
    return;
  }

  BTC::unserialize(src, data.lockTime);
}

// RTT constant factors: K * gamma(1 + 1/K)^K / T^(K-1), where K=6
// Windows: 1 block (150s), 2 blocks (600s), 5 blocks (2400s), 11 blocks (6000s), 17 blocks (9600s)
static constexpr double RttK = 6.0;
static const double RttConstantFactor1  = RttK * std::pow(std::tgamma(1.0 + 1.0 / RttK), RttK) / std::pow(150.0, RttK - 1.0);
static const double RttConstantFactor2  = RttK * std::pow(std::tgamma(1.0 + 1.0 / RttK), RttK) / std::pow(600.0, RttK - 1.0);
static const double RttConstantFactor5  = RttK * std::pow(std::tgamma(1.0 + 1.0 / RttK), RttK) / std::pow(2400.0, RttK - 1.0);
static const double RttConstantFactor11 = RttK * std::pow(std::tgamma(1.0 + 1.0 / RttK), RttK) / std::pow(6000.0, RttK - 1.0);
static const double RttConstantFactor17 = RttK * std::pow(std::tgamma(1.0 + 1.0 / RttK), RttK) / std::pow(9600.0, RttK - 1.0);

static UInt<256> rttComputeNextTarget(int64_t now,
                                      uint32_t prevBits,
                                      const int64_t prevHeaderTime[5],
                                      uint32_t headerBits)
{
  int64_t one = 1;
  UInt<256> prev256 = uint256Compact(prevBits);
  UInt<256> header256 = uint256Compact(headerBits);
  double prevTarget = prev256.getDouble();
  double headerTarget = header256.getDouble();

  // 5 windows: 1, 2, 5, 11, 17 blocks
  static const double factors[5] = {RttConstantFactor1, RttConstantFactor2, RttConstantFactor5, RttConstantFactor11, RttConstantFactor17};
  double nextTarget = headerTarget;
  for (unsigned i = 0; i < 5; i++) {
    int64_t diffTime = std::max(one, now - prevHeaderTime[i]);
    double target = prevTarget * factors[i] * pow(diffTime, RttK - 1.0);
    nextTarget = std::min(nextTarget, target);
  }

  // The real time target is never higher (less difficult) than the normal target.
  if (nextTarget < headerTarget)
    return UInt<256>::fromDouble(nextTarget);
  else
    return header256;
}

void Proto::CheckConsensusCtx::initialize(const CBlockTemplateResult &blockTemplate, const std::string&)
{
  if (!blockTemplate.Rtt.has_value())
    return;

  const CBlockTemplateRtt &rtt = *blockTemplate.Rtt;
  if (rtt.Prevheadertime.size() != 5)
    return;

  for (unsigned i = 0; i < 5; i++)
    PrevHeaderTime[i] = rtt.Prevheadertime[i];

  PrevBits = strtoul(rtt.Prevbits.c_str(), nullptr, 16);
  HasRtt = true;
}

size_t Proto::Transaction::getFirstScriptSigOffset(bool serializeWitness)
{
  size_t result = 0;
  result += 4; //version
  if (serializeWitness && hasWitness()) {
    result += serializedVarSizeLength(0);
    result += 1; // flags
  }

  // txin count
  result += serializedVarSizeLength(txIn.size());
  // txin prefix
  result += txIn[0].scriptSigOffset();
  return result;
}

static inline UInt<256> powValue(const BTC::Proto::BlockHeader &header)
{
  UInt<256> result;
  header.getHash(reinterpret_cast<uint8_t*>(result.data()));
  for (unsigned i = 0; i < 4; i++)
    result.data()[i] = readle(result.data()[i]);
  return result;
}

CCheckStatus Proto::checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams&, const UInt<256> &shareTarget)
{
  CCheckStatus status;
  bool fNegative;
  bool fOverflow;
  status.PowHash = powValue(header);
  UInt<256> defaultTarget = uint256Compact(header.nBits, &fNegative, &fOverflow);

  status.IsShare = status.PowHash <= shareTarget;

  // Check range
  if (fNegative || defaultTarget == 0u || fOverflow)
    return status;

  if (ctx.HasRtt) {
    UInt<256> adjustedTarget = rttComputeNextTarget(time(nullptr), ctx.PrevBits, ctx.PrevHeaderTime, header.nBits);
    status.IsBlock = status.PowHash <= adjustedTarget;
    status.IsPendingBlock = status.PowHash <= defaultTarget;
  } else {
    status.IsBlock = status.PowHash <= defaultTarget;
    status.IsPendingBlock = false;
  }

  // Check proof of work matches claimed amount
  return status;
}

UInt<256> Proto::expectedWork(const Proto::BlockHeader &header, const CheckConsensusCtx &ctx)
{
  if (ctx.HasRtt) {
    UInt<256> adjustedTarget = rttComputeNextTarget(time(nullptr), ctx.PrevBits, ctx.PrevHeaderTime, header.nBits);
    return Stratum::difficultyFromTarget(adjustedTarget);
  } else {
    return Stratum::difficultyFromTarget(uint256Compact(header.nBits));
  }
}

static void processCoinbaseDevReward(const CBlockTemplateResult &blockTemplate, uint64_t *devFee, xmstream &devScriptPubKey)
{
  if (blockTemplate.Coinbasedevreward.has_value()) {
    const CCoinbaseDevReward &devReward = *blockTemplate.Coinbasedevreward;
    *devFee = devReward.Value;
    devScriptPubKey.write(devReward.Scriptpubkey.data(), devReward.Scriptpubkey.size());
  }
}

static void processMinerFund(const CBlockTemplateResult &blockTemplate, uint64_t *blockReward, uint64_t *devFee, xmstream &devScriptPubKey)
{
  if (!blockTemplate.Coinbasetxn.has_value() || !blockTemplate.Coinbasetxn->Minerfund.has_value())
    return;

  const CMinerFund &minerfund = *blockTemplate.Coinbasetxn->Minerfund;
  if (minerfund.Addresses.empty())
    return;

  const std::string &address = minerfund.Addresses[0];
  const char *addrPrefix = nullptr;
  if (strstr(address.c_str(), "bitcoincash:") == address.c_str())
    addrPrefix = "bitcoincash";
  else if (strstr(address.c_str(), "ecash:") == address.c_str())
    addrPrefix = "ecash";
  else if (strstr(address.c_str(), "ectest:") == address.c_str())
    addrPrefix = "ectest";

  if (addrPrefix != nullptr) {
    auto feeAddr = bech32::DecodeCashAddrContent(address.c_str(), addrPrefix);
    if (feeAddr.type == bech32::SCRIPT_TYPE) {
      *devFee = minerfund.Minimumvalue;
      devScriptPubKey.write<uint8_t>(BTC::Script::OP_HASH160);
      devScriptPubKey.write<uint8_t>(0x14);
      devScriptPubKey.write(&feeAddr.hash[0], feeAddr.hash.size());
      devScriptPubKey.write<uint8_t>(BTC::Script::OP_EQUAL);
      *blockReward -= *devFee;
    }
  }
}

static void processDevelopmentFund(const CBlockTemplateResult &blockTemplate, uint64_t *blockReward, uint64_t *devFee, xmstream &devScriptPubKey)
{
  if (!blockTemplate.Coinbasetxn.has_value() ||
      !blockTemplate.Coinbasetxn->Developmentfund.has_value() ||
      !blockTemplate.Coinbasetxn->Developmentfundaddress.has_value())
    return;

  const auto &addresses = *blockTemplate.Coinbasetxn->Developmentfundaddress;
  if (addresses.empty())
    return;

  BTC::Proto::AddressTy address;
  if (BTC::Proto::decodeHumanReadableAddress(addresses[0], {5}, address)) {
    *devFee = *blockTemplate.Coinbasetxn->Developmentfund;
    devScriptPubKey.write<uint8_t>(BTC::Script::OP_HASH160);
    devScriptPubKey.write<uint8_t>(0x14);
    devScriptPubKey.write(address.begin(), address.size());
    devScriptPubKey.write<uint8_t>(BTC::Script::OP_EQUAL);
    *blockReward -= *devFee;
  }
}

static void processStakingReward(const CBlockTemplateResult &blockTemplate, uint64_t *blockReward, uint64_t *stakingReward, xmstream &stakingRewardScriptPubkey)
{
  if (!blockTemplate.Coinbasetxn.has_value() || !blockTemplate.Coinbasetxn->Stakingrewards.has_value())
    return;

  const CStakingRewards &rewards = *blockTemplate.Coinbasetxn->Stakingrewards;
  const auto &script = rewards.Payoutscript.Hex;
  stakingRewardScriptPubkey.write(script.data(), script.size());
  *stakingReward = rewards.Minimumvalue;
  *blockReward -= *stakingReward;
}

bool Stratum::HeaderBuilder::build(Proto::BlockHeader &header, uint32_t *jobVersion, CoinbaseTx&, const std::vector<BaseBlob<256> > &, const CBlockTemplateResult &blockTemplate)
{
  header.nVersion = blockTemplate.Version;
  header.hashPrevBlock = blockTemplate.Previousblockhash;
  header.hashMerkleRoot.setNull();
  header.nTime = blockTemplate.Curtime;
  header.nBits = strtoul(blockTemplate.Bits.c_str(), nullptr, 16);
  header.nNonce = 0;
  *jobVersion = header.nVersion;
  return true;
}

bool Stratum::CoinbaseBuilder::prepare(uint64_t *blockReward, const CBlockTemplateResult &blockTemplate)
{
  *blockReward = blockTemplate.Coinbasevalue;

  // "coinbasedevreward" (FreeCash/FCH)
  processCoinbaseDevReward(blockTemplate, &DevFee, DevScriptPubKey);
  // "minerfund" (XEC)
  processMinerFund(blockTemplate, blockReward, &DevFee, DevScriptPubKey);
  // "stakingrewards" (XEC)
  processStakingReward(blockTemplate, blockReward, &StakingReward, StakingRewardScriptPubkey);
  // "developmentfund" (JKC)
  processDevelopmentFund(blockTemplate, blockReward, &DevFee, DevScriptPubKey);
  return true;
}

void Stratum::CoinbaseBuilder::build(int64_t height,
                                     uint64_t blockReward,
                                     void *coinbaseData,
                                     size_t coinbaseSize,
                                     const std::string &coinbaseMessage,
                                     const Proto::AddressTy &miningAddress,
                                     const CMiningConfig &miningCfg,
                                     bool segwitEnabled,
                                     const xmstream &witnessCommitment,
                                     CoinbaseTx &legacy,
                                     CoinbaseTx &witness)
{
  BTC::Proto::Transaction coinbaseTx;

  coinbaseTx.version = segwitEnabled ? 2 : 1;

  // TxIn
  {
    coinbaseTx.txIn.resize(1);
    typename Proto::TxIn &txIn = coinbaseTx.txIn[0];
    txIn.previousOutputHash.setNull();
    txIn.previousOutputIndex = std::numeric_limits<uint32_t>::max();

    if (segwitEnabled) {
      // Witness nonce
      // Use default: 0
      txIn.witnessStack.resize(1);
      txIn.witnessStack[0].resize(32);
      memset(txIn.witnessStack[0].data(), 0, 32);
    }

    // scriptsig
    xmstream scriptsig;
    // Height
    BTC::serializeForCoinbase(scriptsig, height);
    size_t extraDataOffset = scriptsig.offsetOf();
    // Coinbase extra data
    if (coinbaseData)
      scriptsig.write(coinbaseData, coinbaseSize);
    // Coinbase message
    scriptsig.write(coinbaseMessage.data(), coinbaseMessage.size());
    // Extra nonce
    legacy.ExtraNonceOffset = static_cast<unsigned>(scriptsig.offsetOf() + coinbaseTx.getFirstScriptSigOffset(false));
    legacy.ExtraDataOffset = static_cast<unsigned>(extraDataOffset + coinbaseTx.getFirstScriptSigOffset(false));
    witness.ExtraNonceOffset = static_cast<unsigned>(scriptsig.offsetOf() + coinbaseTx.getFirstScriptSigOffset(true));
    witness.ExtraDataOffset = static_cast<unsigned>(extraDataOffset + coinbaseTx.getFirstScriptSigOffset(true));
    scriptsig.reserve(miningCfg.FixedExtraNonceSize + miningCfg.MutableExtraNonceSize);

    xvectorFromStream(std::move(scriptsig), txIn.scriptSig);
    txIn.sequence = std::numeric_limits<uint32_t>::max();
  }

  // TxOut
  {
    typename Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
    txOut.value = blockReward;

    // pkScript (use single P2PKH)
    txOut.pkScript.resize(sizeof(typename Proto::AddressTy) + 5);
    xmstream p2pkh(txOut.pkScript.data(), txOut.pkScript.size());
    p2pkh.write<uint8_t>(BTC::Script::OP_DUP);
    p2pkh.write<uint8_t>(BTC::Script::OP_HASH160);
    p2pkh.write<uint8_t>(sizeof(typename Proto::AddressTy));
    p2pkh.write(miningAddress.begin(), miningAddress.size());
    p2pkh.write<uint8_t>(BTC::Script::OP_EQUALVERIFY);
    p2pkh.write<uint8_t>(BTC::Script::OP_CHECKSIG);
  }

  if (DevFee) {
    typename Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
    txOut.value = DevFee;
    txOut.pkScript.resize(DevScriptPubKey.sizeOf());
    memcpy(txOut.pkScript.begin(), DevScriptPubKey.data(), DevScriptPubKey.sizeOf());
  }

  if (StakingReward) {
    typename Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
    txOut.value = StakingReward;
    txOut.pkScript.resize(StakingRewardScriptPubkey.sizeOf());
    memcpy(txOut.pkScript.begin(), StakingRewardScriptPubkey.data(), StakingRewardScriptPubkey.sizeOf());
  }

  if (segwitEnabled) {
    typename Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
    txOut.value = 0;
    txOut.pkScript.resize(witnessCommitment.sizeOf());
    memcpy(txOut.pkScript.data(), witnessCommitment.data(), witnessCommitment.sizeOf());
  }

  coinbaseTx.lockTime = 0;
  BTC::Io<typename Proto::Transaction>::serialize(legacy.Data, coinbaseTx, false);
  BTC::Io<typename Proto::Transaction>::serialize(witness.Data, coinbaseTx, true);
}

void Stratum::Notify::build(StratumWork *source, typename Proto::BlockHeader &header, uint32_t asicBoostData, CoinbaseTx &legacy, const std::vector<BaseBlob<256> > &merklePath, const CMiningConfig &cfg, bool resetPreviousWork, xmstream &notifyMessage)
{
  {
    notifyMessage.reset();
    JSON::Object root(notifyMessage);
    root.addNull("id");
    root.addString("method", "mining.notify");
    root.addField("params");
    {
      JSON::Array params(notifyMessage);
      {
        // Id
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%" PRIi64 "#%u", source->StratumId_, source->SendCounter_++);
        params.addString(buffer);
      }

      // Previous block
      {
        std::string hash;
        const uint32_t *data = reinterpret_cast<const uint32_t*>(header.hashPrevBlock.begin());
        for (unsigned i = 0; i < 8; i++) {
          hash.append(writeHexBE(data[i], 4));
        }
        params.addString(hash);
      }

      {
        // Coinbase Tx parts
        // Part 1
        params.addHex(legacy.Data.data(), legacy.ExtraNonceOffset);

        // Part 2
        size_t part2Offset = legacy.ExtraNonceOffset + cfg.FixedExtraNonceSize + cfg.MutableExtraNonceSize;
        size_t part2Size = legacy.Data.sizeOf() - legacy.ExtraNonceOffset - (cfg.FixedExtraNonceSize + cfg.MutableExtraNonceSize);
        params.addHex(legacy.Data.data<uint8_t>() + part2Offset, part2Size);
      }

      {
        // Merkle branches
        params.addField();
        {
          JSON::Array branches(notifyMessage);
          for (const auto &hash: merklePath)
            branches.addHex(hash.begin(), hash.size());
        }
      }
      // nVersion from block template (Header.nVersion is mutable, can't use it)
      params.addString(writeHexBE(asicBoostData, sizeof(asicBoostData)));
      // nBits
      params.addString(writeHexBE(header.nBits, sizeof(header.nBits)));
      // nTime
      params.addString(writeHexBE(header.nTime, sizeof(header.nTime)));
      // cleanup
      params.addBoolean(resetPreviousWork);
    }
  }

  notifyMessage.write('\n');
}

bool Stratum::Prepare::prepare(BTC::Proto::BlockHeader &header, uint32_t jobVersion, CoinbaseTx &legacy, CoinbaseTx &witness, const std::vector<BaseBlob<256>> &merklePath, const CWorkerConfig &workerCfg, const CMiningConfig &miningCfg, const CStratumMessage &msg)
{
  if (msg.Submit.BTC.MutableExtraNonce.size() != miningCfg.MutableExtraNonceSize)
    return false;
  if (workerCfg.AsicBoostEnabled && !msg.Submit.BTC.VersionBits.has_value())
    return false;

  // Write target extra nonce to first txin
  {
    uint8_t *scriptSig = legacy.Data.data<uint8_t>() + legacy.ExtraNonceOffset;
    writeBinBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize, scriptSig);
    memcpy(scriptSig + miningCfg.FixedExtraNonceSize, msg.Submit.BTC.MutableExtraNonce.data(), msg.Submit.BTC.MutableExtraNonce.size());
  }
  {
    uint8_t *scriptSig = witness.Data.data<uint8_t>() + witness.ExtraNonceOffset;
    writeBinBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize, scriptSig);
    memcpy(scriptSig + miningCfg.FixedExtraNonceSize, msg.Submit.BTC.MutableExtraNonce.data(), msg.Submit.BTC.MutableExtraNonce.size());
  }

  // Calculate merkle root and build header
  {
    BaseBlob<256> coinbaseTxHash;
    CCtxSha256 sha256;
    sha256Init(&sha256);
    sha256Update(&sha256, legacy.Data.data(), legacy.Data.sizeOf());
    sha256Final(&sha256, coinbaseTxHash.begin());
    sha256Init(&sha256);
    sha256Update(&sha256, coinbaseTxHash.begin(), coinbaseTxHash.size());
    sha256Final(&sha256, coinbaseTxHash.begin());
    header.hashMerkleRoot = calculateMerkleRootWithPath(coinbaseTxHash, &merklePath[0], merklePath.size(), 0);
  }

  header.nTime = msg.Submit.BTC.Time;
  header.nNonce = msg.Submit.BTC.Nonce;
  if (workerCfg.AsicBoostEnabled)
    header.nVersion = (jobVersion & ~workerCfg.VersionMask) | (msg.Submit.BTC.VersionBits.value() & workerCfg.VersionMask);
  else
    header.nVersion = jobVersion;

  return true;
}
}

void serializeJsonInside(xmstream &stream, const BTC::Proto::BlockHeader &header)
{
  serializeJson(stream, "version", header.nVersion); stream.write(',');
  serializeJson(stream, "hashPrevBlock", header.hashPrevBlock); stream.write(',');
  serializeJson(stream, "hashMerkleRoot", header.hashMerkleRoot); stream.write(',');
  serializeJson(stream, "time", header.nTime); stream.write(',');
  serializeJson(stream, "bits", header.nBits); stream.write(',');
  serializeJson(stream, "nonce", header.nNonce);
}

void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::TxIn &txin)
{
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }

  stream.write('{');
  serializeJson(stream, "previousOutputHash", txin.previousOutputHash); stream.write(',');
  serializeJson(stream, "previousOutputIndex", txin.previousOutputIndex); stream.write(',');
  serializeJson(stream, "scriptsig", txin.scriptSig); stream.write(',');
  serializeJson(stream, "sequence", txin.sequence);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::TxOut &txout)
{
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }

  stream.write('{');
  serializeJson(stream, "value", txout.value); stream.write(',');
  serializeJson(stream, "pkscript", txout.pkScript);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::Transaction &data) {
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }

  stream.write('{');
  serializeJson(stream, "version", data.version); stream.write(',');
  serializeJson(stream, "txin", data.txIn); stream.write(',');
  serializeJson(stream, "txout", data.txOut); stream.write(',');
  serializeJson(stream, "lockTime", data.lockTime);
  stream.write('}');
}

std::string BTC::Proto::makeHumanReadableAddress(uint8_t pubkeyAddressPrefix, const BTC::Proto::AddressTy &address)
{
  uint8_t data[sizeof(BTC::Proto::AddressTy) + 5];
  data[0] = pubkeyAddressPrefix;
  memcpy(&data[1], address.begin(), sizeof(BTC::Proto::AddressTy));

  uint8_t sha256[32];
  CCtxSha256 ctx;
  sha256Init(&ctx);
  sha256Update(&ctx, &data[0], sizeof(data) - 4);
  sha256Final(&ctx, sha256);

  sha256Init(&ctx);
  sha256Update(&ctx, sha256, sizeof(sha256));
  sha256Final(&ctx, sha256);

  memcpy(data+1+sizeof(BTC::Proto::AddressTy), sha256, 4);
  return EncodeBase58(data, data+sizeof(data));
}

bool BTC::Proto::decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &prefix, BTC::Proto::AddressTy &address)
{
  std::vector<uint8_t> data;
  if (!DecodeBase58(hrAddress.c_str(), data) ||
      data.size() != (prefix.size() + sizeof(BTC::Proto::AddressTy) + 4))
    return false;

  if (memcmp(&data[0], &prefix[0], prefix.size()) != 0)
    return false;

  uint32_t addrHash;
  memcpy(&addrHash, &data[prefix.size() + 20], 4);

  uint8_t sha256[32];
  CCtxSha256 ctx;
  sha256Init(&ctx);
  sha256Update(&ctx, &data[0], data.size() - 4);
  sha256Final(&ctx, sha256);

  sha256Init(&ctx);
  sha256Update(&ctx, sha256, sizeof(sha256));
  sha256Final(&ctx, sha256);

  if (reinterpret_cast<uint32_t*>(sha256)[0] != addrHash)
    return false;

  memcpy(address.begin(), &data[prefix.size()], sizeof(BTC::Proto::AddressTy));
  return true;
}

bool BTC::Proto::decodeWIF(const std::string &privateKey, const std::vector<uint8_t> &prefix, uint8_t *result)
{
  std::vector<uint8_t> data;
  if (!DecodeBase58(privateKey.c_str(), data) || data.size() < prefix.size())
    return false;
  if (memcmp(&data[0], &prefix[0], prefix.size()) != 0)
    return false;

  uint32_t addrHash;
  size_t hashDataSize;
  if (data.size() == prefix.size() + 32 + 4) {
    memcpy(&addrHash, &data[prefix.size() + 32], 4);
    hashDataSize = prefix.size() + 32;
  } else if (data.size() == prefix.size() + 32 + 1 +4 ) {
    memcpy(&addrHash, &data[prefix.size() + 32 + 1], 4);
    hashDataSize = prefix.size() + 32 + 1;
  } else {
    return false;
  }

  uint8_t sha256[32];
  CCtxSha256 ctx;
  sha256Init(&ctx);
  sha256Update(&ctx, &data[0], hashDataSize);
  sha256Final(&ctx, sha256);

  sha256Init(&ctx);
  sha256Update(&ctx, sha256, sizeof(sha256));
  sha256Final(&ctx, sha256);

  if (reinterpret_cast<uint32_t*>(sha256)[0] != addrHash)
    return false;

  memcpy(result, &data[prefix.size()], 32);
  return true;
}
