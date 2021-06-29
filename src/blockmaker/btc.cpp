// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockmaker/btc.h"
#include "blockmaker/serializeJson.h"
#include "poolcommon/arith_uint256.h"
#include "poolcore/base58.h"
#include "poolcommon/bech32.h"

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

bool Proto::checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx&, ChainParams&, double *shareDiff)
{
  bool fNegative;
  bool fOverflow;
  arith_uint256 bnTarget;
  bnTarget.SetCompact(header.nBits, &fNegative, &fOverflow);

  arith_uint256 hash = UintToArith256(header.GetHash());
  *shareDiff = getDifficulty(hash.GetCompact());

  // Check range
  if (fNegative || bnTarget == 0 || fOverflow)
    return false;

  // Check proof of work matches claimed amount
  if (hash > bnTarget)
    return false;

  return true;
}

static void processCoinbaseDevReward(rapidjson::Value &blockTemplate, int64_t *devFee, xmstream &devScriptPubKey)
{
  if (blockTemplate.HasMember("coinbasedevreward") && blockTemplate["coinbasedevreward"].IsObject()) {
    rapidjson::Value &devReward = blockTemplate["coinbasedevreward"];
    if (devReward.HasMember("value") && devReward["value"].IsInt64() &&
        devReward.HasMember("scriptpubkey") && devReward["scriptpubkey"].IsString()) {
      *devFee = devReward["value"].GetInt64();
      size_t scriptPubKeyLength = devReward["scriptpubkey"].GetStringLength();
      hex2bin(devReward["scriptpubkey"].GetString(), scriptPubKeyLength, devScriptPubKey.reserve<uint8_t>(scriptPubKeyLength/2));
    }
  }
}

static void processMinerFund(rapidjson::Value &blockTemplate, int64_t *blockReward, int64_t *devFee, xmstream &devScriptPubKey)
{
  if (blockTemplate.HasMember("coinbasetxn") && blockTemplate["coinbasetxn"].IsObject()) {
    rapidjson::Value &coinbasetxn = blockTemplate["coinbasetxn"];
    if (coinbasetxn.HasMember("minerfund") && coinbasetxn["minerfund"].IsObject()) {
      rapidjson::Value &minerfund = coinbasetxn["minerfund"];
      if (minerfund.HasMember("addresses") && minerfund["addresses"].IsArray() &&
          minerfund.HasMember("minimumvalue") && minerfund["minimumvalue"].IsInt64()) {
        rapidjson::Value &addresses = minerfund["addresses"];
        rapidjson::Value &minimumvalue = minerfund["minimumvalue"];
        if (addresses.Size() >= 1 && addresses[0].IsString() && strstr(addresses[0].GetString(), "bitcoincash:") == addresses[0]) {
          // Decode bch bech32 address
          auto feeAddr = bech32::DecodeCashAddrContent(addresses[0].GetString(), "bitcoincash");
          if (feeAddr.type == bech32::SCRIPT_TYPE) {
            *devFee = minimumvalue.GetInt64();
            devScriptPubKey.write<uint8_t>(BTC::Script::OP_HASH160);
            devScriptPubKey.write<uint8_t>(0x14);
            devScriptPubKey.write(&feeAddr.hash[0], feeAddr.hash.size());
            devScriptPubKey.write<uint8_t>(BTC::Script::OP_EQUAL);
            *blockReward -= *devFee;
          }
        }
      }
    }
  }
}

bool Stratum::HeaderBuilder::build(Proto::BlockHeader &header, uint32_t *jobVersion, rapidjson::Value &blockTemplate)
{
  // Check fields:
  // header:
  //   version
  //   previousblockhash
  //   curtime
  //   bits
  if (!blockTemplate.HasMember("version") ||
      !blockTemplate.HasMember("previousblockhash") ||
      !blockTemplate.HasMember("curtime") ||
      !blockTemplate.HasMember("bits")) {
    return false;
  }

  rapidjson::Value &version = blockTemplate["version"];
  rapidjson::Value &hashPrevBlock = blockTemplate["previousblockhash"];
  rapidjson::Value &curtime = blockTemplate["curtime"];
  rapidjson::Value &bits = blockTemplate["bits"];

  header.nVersion = version.GetUint();
  header.hashPrevBlock.SetHex(hashPrevBlock.GetString());
  header.hashMerkleRoot.SetNull();
  header.nTime = curtime.GetUint();
  header.nBits = strtoul(bits.GetString(), nullptr, 16);
  header.nNonce = 0;
  *jobVersion = header.nVersion;
  return true;
}

bool Stratum::CoinbaseBuilder::prepare(int64_t *blockReward,
                                       int64_t *devFee,
                                       xmstream &devScriptPubKey,
                                       rapidjson::Value &blockTemplate)
{
  if (!blockTemplate.HasMember("coinbasevalue"))
    return false;

  rapidjson::Value &coinbaseValue = blockTemplate["coinbasevalue"];
  if (!coinbaseValue.IsInt64())
    return false;

  *blockReward = coinbaseValue.GetInt64();

  // "coinbasedevreward" (FreeCash/FCH)
  processCoinbaseDevReward(blockTemplate, devFee, devScriptPubKey);
  // "minerfund" (BCHA)
  processMinerFund(blockTemplate, blockReward, devFee, devScriptPubKey);
  return true;
}

void Stratum::CoinbaseBuilder::build(int64_t height,
                                     int64_t blockReward,
                                     void *coinbaseData,
                                     size_t coinbaseSize,
                                     const std::string &coinbaseMessage,
                                     const Proto::AddressTy &miningAddress,
                                     const MiningConfig &miningCfg,
                                     int64_t devFeeAmount,
                                     const xmstream &devScriptPubKey,
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
    txIn.previousOutputHash.SetNull();
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

  if (devFeeAmount) {
    typename Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
    txOut.value = devFeeAmount;
    txOut.pkScript.resize(devScriptPubKey.sizeOf());
    memcpy(txOut.pkScript.begin(), devScriptPubKey.data(), devScriptPubKey.sizeOf());
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

void Stratum::Notify::build(CWork *source, typename Proto::BlockHeader &header, uint32_t asicBoostData, CoinbaseTx &legacy, const std::vector<uint256> &merklePath, const MiningConfig &cfg, bool resetPreviousWork, xmstream &notifyMessage)
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

bool Stratum::Prepare::prepare(BTC::Proto::BlockHeader &header, uint32_t jobVersion, CoinbaseTx &legacy, CoinbaseTx &witness, const std::vector<uint256> &merklePath, const CWorkerConfig &workerCfg, const MiningConfig &miningCfg, const StratumMessage &msg)
{
  if (msg.Submit.MutableExtraNonce.size() != miningCfg.MutableExtraNonceSize)
    return false;
  if (workerCfg.AsicBoostEnabled && !msg.Submit.VersionBits.has_value())
    return false;

  // Write target extra nonce to first txin
  {
    uint8_t *scriptSig = legacy.Data.data<uint8_t>() + legacy.ExtraNonceOffset;
    writeBinBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize, scriptSig);
    memcpy(scriptSig + miningCfg.FixedExtraNonceSize, msg.Submit.MutableExtraNonce.data(), msg.Submit.MutableExtraNonce.size());
  }
  {
    uint8_t *scriptSig = witness.Data.data<uint8_t>() + witness.ExtraNonceOffset;
    writeBinBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize, scriptSig);
    memcpy(scriptSig + miningCfg.FixedExtraNonceSize, msg.Submit.MutableExtraNonce.data(), msg.Submit.MutableExtraNonce.size());
  }

  // Calculate merkle root and build header
  header.hashMerkleRoot = calculateMerkleRoot(legacy.Data.data(), legacy.Data.sizeOf(), merklePath);
  header.nTime = msg.Submit.Time;
  header.nNonce = msg.Submit.Nonce;
  if (workerCfg.AsicBoostEnabled)
    header.nVersion = (jobVersion & ~workerCfg.VersionMask) | (msg.Submit.VersionBits.value() & workerCfg.VersionMask);
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


bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &prefix, BTC::Proto::AddressTy &address)
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
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, &data[0], data.size() - 4);
  SHA256_Final(sha256, &ctx);

  SHA256_Init(&ctx);
  SHA256_Update(&ctx, sha256, sizeof(sha256));
  SHA256_Final(sha256, &ctx);

  if (reinterpret_cast<uint32_t*>(sha256)[0] != addrHash)
    return false;

  memcpy(address.begin(), &data[prefix.size()], sizeof(BTC::Proto::AddressTy));
  return true;
}
