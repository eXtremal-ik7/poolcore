// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockmaker/btc.h"
#include "blockmaker/serializeJson.h"
#include "poolcore/base58.h"
#include "poolcommon/arith_uint256.h"
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

void Io<Proto::TxIn>::unpack(xmstream &src, DynamicPtr<BTC::Proto::TxIn> dst)
{
  {
    BTC::Proto::TxIn *ptr = dst.ptr();
    BTC::unserialize(src, ptr->previousOutputHash);
    BTC::unserialize(src, ptr->previousOutputIndex);
  }

  BTC::unpack(src, DynamicPtr<decltype (dst->scriptSig)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxIn, scriptSig)));
  {
    BTC::Proto::TxIn *ptr = dst.ptr();
    new (&ptr->witnessStack) decltype(ptr->witnessStack)();
    BTC::unserialize(src, ptr->sequence);
  }
}

void Io<Proto::TxIn>::unpackFinalize(DynamicPtr<BTC::Proto::TxIn> dst)
{
  BTC::unpackFinalize(DynamicPtr<decltype (dst->scriptSig)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxIn, scriptSig)));
  BTC::unpackFinalize(DynamicPtr<decltype (dst->witnessStack)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxIn, witnessStack)));
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

void Io<Proto::TxOut>::unpack(xmstream &src, DynamicPtr<BTC::Proto::TxOut> dst)
{
  BTC::unserialize(src, dst->value);
  BTC::unpack(src, DynamicPtr<decltype (dst->pkScript)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxOut, pkScript)));
}

void Io<Proto::TxOut>::unpackFinalize(DynamicPtr<BTC::Proto::TxOut> dst)
{
  BTC::unpackFinalize(DynamicPtr<decltype (dst->pkScript)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxOut, pkScript)));
}

void Io<Proto::Transaction>::serialize(xmstream &dst, const BTC::Proto::Transaction &data)
{
  uint8_t flags = 0;
  BTC::serialize(dst, data.version);
  if (data.hasWitness()) {
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

void Io<Proto::Transaction>::unpack(xmstream &src, DynamicPtr<BTC::Proto::Transaction> dst)
{
  uint8_t flags = 0;

  BTC::unserialize(src, dst->version);
  BTC::unpack(src, DynamicPtr<decltype(dst->txIn)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::Transaction, txIn)));

  if (dst->txIn.empty()) {
    BTC::unserialize(src, flags);
    if (flags) {
      BTC::unpack(src, DynamicPtr<decltype(dst->txIn)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::Transaction, txIn)));
      BTC::unpack(src, DynamicPtr<decltype(dst->txOut)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::Transaction, txOut)));
    }
  } else {
    BTC::unpack(src, DynamicPtr<decltype(dst->txOut)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::Transaction, txOut)));
  }

  if (flags & 1) {
    flags ^= 1;

    bool hasWitness = false;
    for (size_t i = 0, ie = dst->txIn.size(); i < ie; i++) {
      size_t txInDataOffset = reinterpret_cast<size_t>(dst->txIn.data());
      size_t txInOffset = txInDataOffset + sizeof(BTC::Proto::TxIn)*i;
      hasWitness |= BTC::Proto::TxIn::unpackWitnessStack(src, DynamicPtr<Proto::TxIn>(dst.stream(), txInOffset));
    }

    if (!hasWitness) {
      src.seekEnd(0, true);
      return;
    }
  }

  if (flags) {
    src.seekEnd(0, true);
    return;
  }

  BTC::unserialize(src, dst->lockTime);
}

size_t Proto::Transaction::getFirstScriptSigOffset()
{
  size_t result = 0;
  result += 4; //version
  if (hasWitness()) {
    result += serializedVarSizeLength(0);
    result += 1; // flags
  }

  // txin count
  result += serializedVarSizeLength(txIn.size());
  // txin prefix
  result += txIn[0].scriptSigOffset();
  return result;
}

static inline double getDifficulty(uint32_t bits)
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

void Stratum::initializeMiningConfig(MiningConfig &cfg, rapidjson::Value &instanceCfg)
{
  if (instanceCfg.HasMember("fixedExtraNonceSize") && instanceCfg["fixedExtraNonceSize"].IsUint())
    cfg.FixedExtraNonceSize = instanceCfg["fixedExtraNonceSize"].GetUint();
  if (instanceCfg.HasMember("mutableExtraNonceSize") && instanceCfg["mutableExtraNonceSize"].IsUint())
    cfg.MutableExtraNonceSize = instanceCfg["mutableExtraNonceSize"].GetUint();
}

void Stratum::initializeThreadConfig(ThreadConfig &cfg, unsigned threadId, unsigned threadsNum)
{
  cfg.ExtraNonceCurrent = threadId;
  cfg.ThreadsNum = threadsNum;
}

void Stratum::initializeWorkerConfig(WorkerConfig &cfg, ThreadConfig &threadCfg)
{
  // Set fixed part of extra nonce
  cfg.ExtraNonceFixed = threadCfg.ExtraNonceCurrent;

  // Set session names
  uint8_t sessionId[16];
  {
    RAND_bytes(sessionId, sizeof(sessionId));
    cfg.SetDifficultySession.resize(sizeof(sessionId)*2);
    bin2hexLowerCase(sessionId, cfg.SetDifficultySession.data(), sizeof(sessionId));
  }
  {
    RAND_bytes(sessionId, sizeof(sessionId));
    cfg.NotifySession.resize(sizeof(sessionId)*2);
    bin2hexLowerCase(sessionId, cfg.NotifySession.data(), sizeof(sessionId));
  }

  // Update thread config
  threadCfg.ExtraNonceCurrent += threadCfg.ThreadsNum;
}

void Stratum::buildNotifyMessage(Work &work, MiningConfig &cfg, uint64_t majorJobId, unsigned minorJobId, bool resetPreviousWork, xmstream &out)
{
  {
    out.reset();
    JSON::Object root(out);
    root.addNull("id");
    root.addString("method", "mining.notify");
    root.addField("params");
    {
      JSON::Array params(out);
      {
        // Id
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%" PRIu64 "#%u", majorJobId, minorJobId);
        params.addString(buffer);
      }

      // Previous block
      {
        std::string hash;
        const uint32_t *data = reinterpret_cast<const uint32_t*>(work.Header.hashPrevBlock.begin());
        for (unsigned i = 0; i < 8; i++) {
          hash.append(writeHexBE(data[i], 4));
        }
        params.addString(hash);
      }

      {
        // Coinbase Tx parts
        // Part 1
        params.addHex(work.FirstTxData.data(), work.TxExtraNonceOffset);

        // Part 2
        size_t part2Offset = work.TxExtraNonceOffset + cfg.FixedExtraNonceSize + cfg.MutableExtraNonceSize;
        size_t part2Size = work.FirstTxData.sizeOf() - work.TxExtraNonceOffset - (cfg.FixedExtraNonceSize + cfg.MutableExtraNonceSize);
        params.addHex(work.FirstTxData.data<uint8_t>() + part2Offset, part2Size);
      }

      {
        // Merkle branches
        params.addField();
        {
          JSON::Array branches(out);
          for (const auto &hash: work.MerklePath)
            branches.addHex(hash.begin(), hash.size());
        }
      }
      // nVersion
      params.addString(writeHexBE(work.Header.nVersion, sizeof(work.Header.nVersion)));
      // nBits
      params.addString(writeHexBE(work.Header.nBits, sizeof(work.Header.nBits)));
      // nTime
      params.addString(writeHexBE(work.Header.nTime, sizeof(work.Header.nTime)));
      // cleanup
      params.addBoolean(resetPreviousWork);
    }
  }

  out.write('\n');
}

static inline void addId(JSON::Object &object, StratumMessage &msg) {
  if (!msg.stringId.empty())
    object.addString("id", msg.stringId);
  else
    object.addInt("id", msg.integerId);
}

void Stratum::onSubscribe(MiningConfig &miningCfg, WorkerConfig &workerCfg, StratumMessage &msg, xmstream &out)
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
          setDifficultySession.addString(workerCfg.SetDifficultySession);
        }
        sessions.addField();
        {
          JSON::Array notifySession(out);
          notifySession.addString("mining.notify");
          notifySession.addString(workerCfg.NotifySession);
        }
      }

      // Unique extra nonce
      result.addString(writeHexBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize));
      // Mutable part of extra nonce size
      result.addInt(miningCfg.MutableExtraNonceSize);
    }
    object.addNull("error");
  }

  out.write('\n');
}

bool Stratum::loadFromTemplate(Work &work,
                               rapidjson::Value &document,
                               uint64_t uniqueWorkId,
                               const MiningConfig &cfg,
                               PoolBackend *backend,
                               const std::string&,
                               Proto::AddressTy &miningAddress,
                               const std::string &coinbaseMsg,
                               std::string &error)
{
  char buffer[4096];
  xmstream stream(buffer, sizeof(buffer));
  work.Height = 0;
  work.UniqueWorkId = uniqueWorkId;
  work.MerklePath.clear();
  work.FirstTxData.reset();
  work.BlockHexData.reset();
  work.NotifyMessage.reset();
  work.Backend = backend;

  if (!document.HasMember("result") || !document["result"].IsObject()) {
    error = "no result";
    return false;
  }

  rapidjson::Value &blockTemplate = document["result"];

  // Check segwit enabled (rules array)
  bool segwitEnabled = false;
  if (blockTemplate.HasMember("rules") && blockTemplate["rules"].IsArray()) {
    rapidjson::Value::Array rules = blockTemplate["rules"].GetArray();
    for (rapidjson::SizeType i = 0, ie = rules.Size(); i != ie; ++i) {
      if (rules[i].IsString() && strcmp(rules[i].GetString(), "!segwit") == 0) {
        segwitEnabled = true;
        break;
      }
    }
  }

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

  work.Height = height.GetUint64();
  Proto::BlockHeader &header = work.Header;
  header.nVersion = version.GetUint();
  header.hashPrevBlock.SetHex(hashPrevBlock.GetString());
  header.hashMerkleRoot.SetNull();
  header.nTime = curtime.GetUint();
  header.nBits = strtoul(bits.GetString(), nullptr, 16);
  header.nNonce = 0;

  // Serialize header and transactions count
  stream.reset();
  BTC::X::serialize(stream, header);
  serializeVarSize(stream, transactions.Size() + 1);
  bin2hexLowerCase(stream.data(), work.BlockHexData.reserve<char>(stream.sizeOf()*2), stream.sizeOf());

  // Coinbase
  Proto::Transaction coinbaseTx;
  coinbaseTx.version = segwitEnabled ? 2 : 1;

  // TxIn
  {
    coinbaseTx.txIn.resize(1);
    BTC::Proto::TxIn &txIn = coinbaseTx.txIn[0];
    txIn.previousOutputHash.SetNull();
    txIn.previousOutputIndex = std::numeric_limits<uint32_t>::max();

    // scriptsig
    xmstream scriptsig;
    // Height
    BTC::serializeForCoinbase(scriptsig, work.Height);
    // Coinbase message
    scriptsig.write(coinbaseMsg.data(), coinbaseMsg.size());
    // Extra nonce
    work.ScriptSigExtraNonceOffset = scriptsig.offsetOf();
    work.TxExtraNonceOffset = work.ScriptSigExtraNonceOffset + coinbaseTx.getFirstScriptSigOffset();
    for (size_t i = 0, ie = cfg.FixedExtraNonceSize+cfg.MutableExtraNonceSize; i != ie; ++i)
      scriptsig.write('\0');

    xvectorFromStream(std::move(scriptsig), txIn.scriptSig);
    txIn.sequence = std::numeric_limits<uint32_t>::max();
  }

  // TxOut
  {
    coinbaseTx.txOut.resize(2);

    {
      // First txout (funds)
      BTC::Proto::TxOut &txOut = coinbaseTx.txOut[0];
      work.BlockReward = txOut.value = coinbaseValue.GetInt64();

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

    if (segwitEnabled) {
      // Second txout (witness)
      BTC::Proto::TxOut &txOut = coinbaseTx.txOut[1];

      if (!blockTemplate.HasMember("default_witness_commitment") || !blockTemplate["default_witness_commitment"].IsString()) {
        error = "default_witness_commitment missing";
        return false;
      }

      const char *witnessCommitmentData = blockTemplate["default_witness_commitment"].GetString();
      size_t witnessCommitmentDataSize = blockTemplate["default_witness_commitment"].GetStringLength();
      txOut.value = 0;
      txOut.pkScript.resize(witnessCommitmentDataSize/2);
      hex2bin(witnessCommitmentData, witnessCommitmentDataSize, txOut.pkScript.data());
    }
  }

  coinbaseTx.lockTime = 0;
  work.BlockHexExtraNonceOffset = work.BlockHexData.sizeOf() + work.TxExtraNonceOffset*2;
  BTC::X::serialize(work.FirstTxData, coinbaseTx);
  bin2hexLowerCase(work.FirstTxData.data(), work.BlockHexData.reserve<char>(work.FirstTxData.sizeOf()*2), work.FirstTxData.sizeOf());

  // Transactions
  std::vector<uint256> txHashes;
  txHashes.emplace_back();
  txHashes.back().SetNull();
  for (rapidjson::SizeType i = 0, ie = transactions.Size(); i != ie; ++i) {
    rapidjson::Value &txSrc = transactions[i];
    if (!txSrc.HasMember("data") || !txSrc["data"].IsString()) {
      error = "no 'data' for transaction";
      return false;
    }

    work.BlockHexData.write(txSrc["data"].GetString(), txSrc["data"].GetStringLength());
    if (!txSrc.HasMember("txid") || !txSrc["txid"].IsString()) {
      error = "no 'hash' for transaction";
      return false;
    }

    txHashes.emplace_back();
    txHashes.back().SetHex(txSrc["txid"].GetString());
  }

  work.TxNum = txHashes.size();

  // Build merkle path
  dumpMerkleTree(txHashes, work.MerklePath);
  return true;
}

bool Stratum::prepareForSubmit(Work &work, const WorkerConfig &workerCfg, const MiningConfig &miningCfg, const StratumMessage &msg)
{
  if (msg.submit.MutableExtraNonce.size() != miningCfg.MutableExtraNonceSize)
    return false;

  Proto::BlockHeader &header = work.Header;

  // Write target extra nonce to first txin
  uint8_t *scriptSig = work.FirstTxData.data<uint8_t>() + work.TxExtraNonceOffset;
  writeBinBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize, scriptSig);
  memcpy(scriptSig + miningCfg.FixedExtraNonceSize, msg.submit.MutableExtraNonce.data(), msg.submit.MutableExtraNonce.size());
  // Update extra nonce in block hex dump
  bin2hexLowerCase(scriptSig, work.BlockHexData.data<char>() + work.BlockHexExtraNonceOffset, miningCfg.FixedExtraNonceSize + miningCfg.MutableExtraNonceSize);

  // Calculate merkle root and build header
  header.hashMerkleRoot = calculateMerkleRoot(work.FirstTxData.data(), work.FirstTxData.sizeOf(), work.MerklePath);
  header.nTime = msg.submit.Time;
  header.nNonce = msg.submit.Nonce;

  {
    // Update header in block hex dump
    char buffer[128];
    xmstream stream(buffer, sizeof(buffer));
    stream.reset();
    BTC::X::serialize(stream, header);
    bin2hexLowerCase(buffer, work.BlockHexData.data<char>(), stream.sizeOf());
  }

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
