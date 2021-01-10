// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockmaker/btc.h"
#include "blockmaker/serializeJson.h"
#include "poolcore/base58.h"
#include "poolcommon/arith_uint256.h"
#include "poolcommon/bech32.h"
#include "poolcommon/jsonSerializer.h"
#include "poolcommon/utils.h"
#include "blockmaker/merkleTree.h"
#include <openssl/rand.h>
#include <memory>

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

double getDifficulty(uint32_t bits)
{
    int nShift = (bits >> 24) & 0xff;
    double dDiff =
        (double)0x0000ffff / (double)(bits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
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

void Stratum::MiningConfig::initialize(rapidjson::Value &instanceCfg)
{
  if (instanceCfg.HasMember("fixedExtraNonceSize") && instanceCfg["fixedExtraNonceSize"].IsUint())
    FixedExtraNonceSize = instanceCfg["fixedExtraNonceSize"].GetUint();
  if (instanceCfg.HasMember("mutableExtraNonceSize") && instanceCfg["mutableExtraNonceSize"].IsUint())
    MutableExtraNonceSize = instanceCfg["mutableExtraNonceSize"].GetUint();
}

void Stratum::ThreadConfig::initialize(unsigned int instanceId, unsigned int instancesNum)
{
  ExtraNonceCurrent = instanceId;
  ThreadsNum = instancesNum;
}

void Stratum::WorkerConfig::initialize(ThreadConfig &threadCfg)
{
  // Set fixed part of extra nonce
  ExtraNonceFixed = threadCfg.ExtraNonceCurrent;

  // Set session names
  uint8_t sessionId[16];
  {
    RAND_bytes(sessionId, sizeof(sessionId));
    SetDifficultySession.resize(sizeof(sessionId)*2);
    bin2hexLowerCase(sessionId, SetDifficultySession.data(), sizeof(sessionId));
  }
  {
    RAND_bytes(sessionId, sizeof(sessionId));
    NotifySession.resize(sizeof(sessionId)*2);
    bin2hexLowerCase(sessionId, NotifySession.data(), sizeof(sessionId));
  }

  // Update thread config
  threadCfg.ExtraNonceCurrent += threadCfg.ThreadsNum;
}

void Stratum::WorkerConfig::setupVersionRolling(uint32_t versionMask)
{
  AsicBoostEnabled = true;
  VersionMask = versionMask;
}

double Stratum::Work::expectedWork(size_t)
{
  return getDifficulty(Header.nBits);
}

void Stratum::Work::buildNotifyMessage(MiningConfig &cfg, uint64_t majorJobId, unsigned int minorJobId, bool resetPreviousWork)
{
  {
    NotifyMessage.reset();
    JSON::Object root(NotifyMessage);
    root.addNull("id");
    root.addString("method", "mining.notify");
    root.addField("params");
    {
      JSON::Array params(NotifyMessage);
      {
        // Id
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%" PRIu64 "#%u", majorJobId, minorJobId);
        params.addString(buffer);
      }

      // Previous block
      {
        std::string hash;
        const uint32_t *data = reinterpret_cast<const uint32_t*>(Header.hashPrevBlock.begin());
        for (unsigned i = 0; i < 8; i++) {
          hash.append(writeHexBE(data[i], 4));
        }
        params.addString(hash);
      }

      {
        // Coinbase Tx parts
        // Part 1
        params.addHex(CBTxLegacy.Data.data(), CBTxLegacy.ExtraNonceOffset);

        // Part 2
        size_t part2Offset = CBTxLegacy.ExtraNonceOffset + cfg.FixedExtraNonceSize + cfg.MutableExtraNonceSize;
        size_t part2Size = CBTxLegacy.Data.sizeOf() - CBTxLegacy.ExtraNonceOffset - (cfg.FixedExtraNonceSize + cfg.MutableExtraNonceSize);
        params.addHex(CBTxLegacy.Data.data<uint8_t>() + part2Offset, part2Size);
      }

      {
        // Merkle branches
        params.addField();
        {
          JSON::Array branches(NotifyMessage);
          for (const auto &hash: MerklePath)
            branches.addHex(hash.begin(), hash.size());
        }
      }
      // nVersion from block template (Header.nVersion is mutable, can't use it)
      params.addString(writeHexBE(JobVersion, sizeof(JobVersion)));
      // nBits
      params.addString(writeHexBE(Header.nBits, sizeof(Header.nBits)));
      // nTime
      params.addString(writeHexBE(Header.nTime, sizeof(Header.nTime)));
      // cleanup
      params.addBoolean(resetPreviousWork);
    }
  }

  NotifyMessage.write('\n');
}

static inline void addId(JSON::Object &object, StratumMessage &msg) {
  if (!msg.stringId.empty())
    object.addString("id", msg.stringId);
  else
    object.addInt("id", msg.integerId);
}

void Stratum::WorkerConfig::onSubscribe(MiningConfig &miningCfg, StratumMessage &msg, xmstream &out, std::string &subscribeInfo)
{
  // Response format
  // {"id": 1, "result": [ [ ["mining.set_difficulty", <setDifficultySession>:string(hex)], ["mining.notify", <notifySession>:string(hex)]], <uniqueExtraNonce>:string(hex), extraNonceSize:integer], "error": null}\n
  {
    JSON::Object object(out);
    addId(object, msg);
    object.addField("result");
    {
      JSON::Array result(out);
      result.addField();
      {
        JSON::Array sessions(out);
        sessions.addField();
        {
          JSON::Array setDifficultySession(out);
          setDifficultySession.addString("mining.set_difficulty");
          setDifficultySession.addString(SetDifficultySession);
        }
        sessions.addField();
        {
          JSON::Array notifySession(out);
          notifySession.addString("mining.notify");
          notifySession.addString(NotifySession);
        }
      }

      // Unique extra nonce
      result.addString(writeHexBE(ExtraNonceFixed, miningCfg.FixedExtraNonceSize));
      // Mutable part of extra nonce size
      result.addInt(miningCfg.MutableExtraNonceSize);
    }
    object.addNull("error");
  }

  out.write('\n');
  subscribeInfo = std::to_string(ExtraNonceFixed);
}

bool Stratum::Work::loadFromTemplate(rapidjson::Value &document,
                                     uint64_t uniqueWorkId,
                                     const MiningConfig &cfg,
                                     PoolBackend *backend,
                                     const std::string &ticker,
                                     Proto::AddressTy &miningAddress,
                                     const void *coinBaseExtraData,
                                     size_t coinbaseExtraSize,
                                     std::string &error,
                                     uint32_t txNumLimit)
{
  char buffer[4096];
  xmstream stream(buffer, sizeof(buffer));
  Height = 0;
  UniqueWorkId = uniqueWorkId;
  MerklePath.clear();
  CBTxLegacy.Data.reset();
  CBTxWitness.Data.reset();
  BlockHexData.reset();
  Backend = backend;

  if (!document.HasMember("result") || !document["result"].IsObject()) {
    error = "no result";
    return false;
  }

  rapidjson::Value &blockTemplate = document["result"];

  // Check fields:
  // height
  // header:
  //   version
  //   previousblockhash
  //   curtime
  //   bits
  // transactions
  if (!blockTemplate.HasMember("height") ||
      !blockTemplate.HasMember("version") ||
      !blockTemplate.HasMember("previousblockhash") ||
      !blockTemplate.HasMember("curtime") ||
      !blockTemplate.HasMember("bits") ||
      !blockTemplate.HasMember("coinbasevalue") ||
      !blockTemplate.HasMember("transactions") || !blockTemplate["transactions"].IsArray()) {
    error = "missing data";
    return false;
  }

  rapidjson::Value &height = blockTemplate["height"];
  rapidjson::Value &version = blockTemplate["version"];
  rapidjson::Value &hashPrevBlock = blockTemplate["previousblockhash"];
  rapidjson::Value &curtime = blockTemplate["curtime"];
  rapidjson::Value &bits = blockTemplate["bits"];
  rapidjson::Value &coinbaseValue = blockTemplate["coinbasevalue"];
  rapidjson::Value::Array transactions = blockTemplate["transactions"].GetArray();
  if (!height.IsUint64() ||
      !version.IsUint() ||
      !hashPrevBlock.IsString() ||
      !curtime.IsUint() ||
      !bits.IsString() ||
      !coinbaseValue.IsInt64()) {
    error = "height or header data invalid format";
    return false;
  }

  int64_t blockReward = coinbaseValue.GetInt64();
  int64_t pureBlockReward = blockReward;

  // Check segwit enabled (compare txid and hash for all transactions)
  bool segwitEnabled = false;
  bool txFilter = txNumLimit && transactions.Size() > txNumLimit;
  rapidjson::SizeType txNum = txFilter ? txNumLimit : transactions.Size();
  std::vector<uint256> witnessHashes;
  xmstream witnessCommitment;

  witnessHashes.emplace_back();
  witnessHashes.back().SetNull();
  for (rapidjson::SizeType i = 0, ie = transactions.Size(); i != ie; ++i) {
    rapidjson::Value &tx = transactions[i];
    if (!tx.IsObject()) {
      error = "transaction is not JSON object";
      return false;
    }

    if (!tx.HasMember("fee") || !tx["fee"].IsInt64()) {
      error = "no 'fee' in transaction";
      return false;
    }

    int64_t txFee = tx["fee"].GetInt64();
    pureBlockReward -= txFee;
    if (i >= txNum)
      blockReward -= txFee;

    if (i < txNum &&
        tx.HasMember("txid") && tx["txid"].IsString() &&
        tx.HasMember("hash") && tx["hash"].IsString()) {
      rapidjson::Value &txid = tx["txid"];
      rapidjson::Value &hash = tx["hash"];
      if (txid.GetStringLength() == hash.GetStringLength()) {
        if (memcmp(txid.GetString(), hash.GetString(), txid.GetStringLength()) != 0)
          segwitEnabled = true;
      }

      if (txFilter) {
        witnessHashes.emplace_back();
        witnessHashes.back().SetHex(hash.GetString());
      }
    }
  }

  // "coinbasedevreward" support
  int64_t devFee = 0;
  xmstream devScriptPubKey;
  if (blockTemplate.HasMember("coinbasedevreward") && blockTemplate["coinbasedevreward"].IsObject()) {
    rapidjson::Value &devReward = blockTemplate["coinbasedevreward"];
    if (devReward.HasMember("value") && devReward["value"].IsInt64() &&
        devReward.HasMember("scriptpubkey") && devReward["scriptpubkey"].IsString()) {
      devFee = devReward["value"].GetInt64();
      size_t scriptPubKeyLength = devReward["scriptpubkey"].GetStringLength();
      hex2bin(devReward["scriptpubkey"].GetString(), scriptPubKeyLength, devScriptPubKey.reserve<uint8_t>(scriptPubKeyLength/2));
    }
  }

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
            devFee = minimumvalue.GetInt64();
            devScriptPubKey.write<uint8_t>(BTC::Script::OP_HASH160);
            devScriptPubKey.write<uint8_t>(0x14);
            devScriptPubKey.write(&feeAddr.hash[0], feeAddr.hash.size());
            devScriptPubKey.write<uint8_t>(BTC::Script::OP_EQUAL);
            blockReward -= devFee;
          }
        }
      }
    }
  }

  Proto::AddressTy devFeeAddress;

  if (txFilter)
    LOG_F(INFO, " * [txfilter] transactions num %i -> %i; coinbase value %" PRIi64 " -> %" PRIi64 "", static_cast<int>(transactions.Size()), static_cast<int>(txNum), coinbaseValue.GetInt64(), blockReward);

  if (segwitEnabled) {
    if (!blockTemplate.HasMember("default_witness_commitment") || !blockTemplate["default_witness_commitment"].IsString()) {
      error = "default_witness_commitment missing";
      return false;
    }

    const char *originalWitnessCommitment = blockTemplate["default_witness_commitment"].GetString();
    rapidjson::SizeType originalWitnessCommitmentSize = blockTemplate["default_witness_commitment"].GetStringLength();

    if (!txFilter) {
      hex2bin(originalWitnessCommitment, originalWitnessCommitmentSize, witnessCommitment.reserve(originalWitnessCommitmentSize/2));
    } else {
      // Calculate witness merkle root
      uint256 witnessMerkleRoot = calculateMerkleRoot(&witnessHashes[0], witnessHashes.size());
      // Calculate witness commitment
      uint256 commitment;
      {
        uint8_t defaultWitnessNonce[32];
        memset(defaultWitnessNonce, 0, sizeof(defaultWitnessNonce));
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, witnessMerkleRoot.begin(), witnessMerkleRoot.size());
        SHA256_Update(&ctx, defaultWitnessNonce, 32);
        SHA256_Final(commitment.begin(), &ctx);
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, commitment.begin(), commitment.size());
        SHA256_Final(commitment.begin(), &ctx);
      }

      uint8_t prefix[6] = {0x6A, 0x24, 0xAA, 0x21, 0xA9, 0xED};
      witnessCommitment.write(prefix, sizeof(prefix));
      witnessCommitment.write(commitment.begin(), commitment.size());
    }
  }

  Height = height.GetUint64();
  Proto::BlockHeader &header = Header;
  header.nVersion = version.GetUint();
  header.hashPrevBlock.SetHex(hashPrevBlock.GetString());
  header.hashMerkleRoot.SetNull();
  header.nTime = curtime.GetUint();
  header.nBits = strtoul(bits.GetString(), nullptr, 16);
  header.nNonce = 0;
  JobVersion = header.nVersion;

  // Serialize header and transactions count
  stream.reset();
  BTC::X::serialize(stream, header);
  serializeVarSize(stream, txNum + 1);
  bin2hexLowerCase(stream.data(), BlockHexData.reserve<char>(stream.sizeOf()*2), stream.sizeOf());

  // Coinbase
  Proto::Transaction coinbaseTx;
  coinbaseTx.version = segwitEnabled ? 2 : 1;

  // TxIn
  {
    coinbaseTx.txIn.resize(1);
    BTC::Proto::TxIn &txIn = coinbaseTx.txIn[0];
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
    BTC::serializeForCoinbase(scriptsig, Height);
    size_t extraDataOffset = scriptsig.offsetOf();
    // Coinbase message
    scriptsig.write(coinBaseExtraData, coinbaseExtraSize);
    // Extra nonce
    CBTxLegacy.ExtraNonceOffset = static_cast<unsigned>(scriptsig.offsetOf() + coinbaseTx.getFirstScriptSigOffset(false));
    CBTxLegacy.ExtraDataOffset = static_cast<unsigned>(extraDataOffset + coinbaseTx.getFirstScriptSigOffset(false));
    CBTxWitness.ExtraNonceOffset = static_cast<unsigned>(scriptsig.offsetOf() + coinbaseTx.getFirstScriptSigOffset(true));
    CBTxWitness.ExtraDataOffset = static_cast<unsigned>(extraDataOffset + coinbaseTx.getFirstScriptSigOffset(true));
    scriptsig.reserve(cfg.FixedExtraNonceSize+cfg.MutableExtraNonceSize);

    xvectorFromStream(std::move(scriptsig), txIn.scriptSig);
    txIn.sequence = std::numeric_limits<uint32_t>::max();
  }

  // TxOut
  {
    BTC::Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
    BlockReward = txOut.value = blockReward;

    // pkScript (use single P2PKH)
    txOut.pkScript.resize(sizeof(BTC::Proto::AddressTy) + 5);
    xmstream p2pkh(txOut.pkScript.data(), txOut.pkScript.size());
    p2pkh.write<uint8_t>(BTC::Script::OP_DUP);
    p2pkh.write<uint8_t>(BTC::Script::OP_HASH160);
    p2pkh.write<uint8_t>(sizeof(BTC::Proto::AddressTy));
    p2pkh.write(miningAddress.begin(), miningAddress.size());
    p2pkh.write<uint8_t>(BTC::Script::OP_EQUALVERIFY);
    p2pkh.write<uint8_t>(BTC::Script::OP_CHECKSIG);
  }

  if (devFee) {
    BTC::Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
    txOut.value = devFee;
    txOut.pkScript.resize(devScriptPubKey.sizeOf());
    memcpy(txOut.pkScript.begin(), devScriptPubKey.data(), devScriptPubKey.sizeOf());
  }

  if (segwitEnabled) {
    BTC::Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
    txOut.value = 0;
    txOut.pkScript.resize(witnessCommitment.sizeOf());
    memcpy(txOut.pkScript.data(), witnessCommitment.data(), witnessCommitment.sizeOf());
  }

  coinbaseTx.lockTime = 0;
  BlockHexCoinbaseTxOffset = static_cast<unsigned>(BlockHexData.sizeOf());
  BTC::Io<BTC::Proto::Transaction>::serialize(CBTxLegacy.Data, coinbaseTx, false);
  BTC::Io<BTC::Proto::Transaction>::serialize(CBTxWitness.Data, coinbaseTx, true);
  bin2hexLowerCase(CBTxWitness.Data.data(), BlockHexData.reserve<char>(CBTxWitness.Data.sizeOf()*2), CBTxWitness.Data.sizeOf());

  // Transactions
  std::vector<uint256> txHashes;
  txHashes.emplace_back();
  txHashes.back().SetNull();
  for (rapidjson::SizeType i = 0; i < txNum; i++) {
    rapidjson::Value &txSrc = transactions[i];
    if (!txSrc.HasMember("data") || !txSrc["data"].IsString()) {
      error = "no 'data' for transaction";
      return false;
    }

    BlockHexData.write(txSrc["data"].GetString(), txSrc["data"].GetStringLength());
    if (!txSrc.HasMember("txid") || !txSrc["txid"].IsString()) {
      error = "no 'txid' for transaction";
      return false;
    }

    txHashes.emplace_back();
    txHashes.back().SetHex(txSrc["txid"].GetString());
  }

  TxNum = txNum;

  // Build merkle path
  dumpMerkleTree(txHashes, MerklePath);
  return true;
}

bool Stratum::Work::prepareForSubmit(const WorkerConfig &workerCfg, const MiningConfig &miningCfg, const StratumMessage &msg)
{
  if (msg.submit.MutableExtraNonce.size() != miningCfg.MutableExtraNonceSize)
    return false;
  if (workerCfg.AsicBoostEnabled && !msg.submit.VersionBits.has_value())
    return false;

  Proto::BlockHeader &header = Header;

  // Write target extra nonce to first txin
  {
    uint8_t *scriptSig = CBTxLegacy.Data.data<uint8_t>() + CBTxLegacy.ExtraNonceOffset;
    writeBinBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize, scriptSig);
    memcpy(scriptSig + miningCfg.FixedExtraNonceSize, msg.submit.MutableExtraNonce.data(), msg.submit.MutableExtraNonce.size());
  }
  {
    uint8_t *scriptSig = CBTxWitness.Data.data<uint8_t>() + CBTxWitness.ExtraNonceOffset;
    writeBinBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize, scriptSig);
    memcpy(scriptSig + miningCfg.FixedExtraNonceSize, msg.submit.MutableExtraNonce.data(), msg.submit.MutableExtraNonce.size());
  }

  // Update coinbase tx in block hex dump
  bin2hexLowerCase(CBTxWitness.Data.data(), BlockHexData.data<char>() + BlockHexCoinbaseTxOffset, CBTxWitness.Data.sizeOf());

  // Calculate merkle root and build header
  header.hashMerkleRoot = calculateMerkleRoot(CBTxLegacy.Data.data(), CBTxLegacy.Data.sizeOf(), MerklePath);
  header.nTime = msg.submit.Time;
  header.nNonce = msg.submit.Nonce;
  if (workerCfg.AsicBoostEnabled)
    header.nVersion = (JobVersion & ~workerCfg.VersionMask) | (msg.submit.VersionBits.value() & workerCfg.VersionMask);
  else
    header.nVersion = JobVersion;

  {
    // Update header in block hex dump
    char buffer[128];
    xmstream stream(buffer, sizeof(buffer));
    stream.reset();
    BTC::X::serialize(stream, header);
    bin2hexLowerCase(buffer, BlockHexData.data<char>(), stream.sizeOf());
  }

  return true;
}

double Stratum::getIncomingProfitValue(rapidjson::Value &document, double price, double coeff)
{
  if (!document.HasMember("result") || !document["result"].IsObject()) {
    return 0.0;
  }

  rapidjson::Value &blockTemplate = document["result"];

  // Check fields:
  // height
  // header:
  //   version
  //   previousblockhash
  //   curtime
  //   bits
  // transactions
  if (!blockTemplate.HasMember("bits") ||
      !blockTemplate.HasMember("coinbasevalue")) {
    return 0.0;
  }

  rapidjson::Value &bits = blockTemplate["bits"];
  rapidjson::Value &coinbaseValue = blockTemplate["coinbasevalue"];
  if (!bits.IsString() ||
      !coinbaseValue.IsInt64()) {
    return 0.0;
  }

  uint32_t bitsInt = strtoul(bits.GetString(), nullptr, 16);
  return price * coinbaseValue.GetInt64() / getDifficulty(bitsInt) * coeff;
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
