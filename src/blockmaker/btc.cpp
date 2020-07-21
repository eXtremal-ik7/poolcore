// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockmaker/btc.h"
#include "blockmaker/serializeJson.h"
#include "poolcore/base58.h"

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

bool Proto::loadHeaderFromTemplate(BTC::Proto::BlockHeader &header, rapidjson::Value &blockTemplate)
{
  // version
  if (!blockTemplate.HasMember("version") || !blockTemplate["version"].IsUint())
    return false;
  header.nVersion = blockTemplate["version"].GetUint();

  // hashPrevBlock
  if (!blockTemplate.HasMember("previousblockhash") || !blockTemplate["previousblockhash"].IsString())
    return false;
  header.hashPrevBlock.SetHex(blockTemplate["previousblockhash"].GetString());

  // hashMerkleRoot
  header.hashMerkleRoot.SetNull();

  // time
  if (!blockTemplate.HasMember("mintime") || !blockTemplate["mintime"].IsUint())
    return false;
  header.nTime = blockTemplate["mintime"].GetUint();

  // bits
  if (!blockTemplate.HasMember("bits") || !blockTemplate["bits"].IsString())
    return false;
  header.nBits = strtoul(blockTemplate["bits"].GetString(), nullptr, 16);

  // nonce
  header.nNonce = 0;
  return true;
}
}

static inline uint8_t hexLowerCaseDigit(char c)
{
  uint8_t digit = c - '0';
  if (digit >= 10)
    digit -= ('a' - '0' - 10);
  return digit;
}

static inline void hexLowerCase2bin(const char *in, size_t inSize, void *out)
{
  uint8_t *pOut = static_cast<uint8_t*>(out);
  for (size_t i = 0; i < inSize/2; i++)
    pOut[i] = (hexLowerCaseDigit(in[i*2]) << 4) | hexLowerCaseDigit(in[i*2+1]);
}

bool loadTransactionsFromTemplate(xvector<BTC::Proto::Transaction> &vtx, rapidjson::Value &blockTemplate, xmstream &buffer)
{
  if (!blockTemplate.HasMember("transactions") || !blockTemplate["transactions"].IsArray())
    return false;
  rapidjson::Value::Array transactions = blockTemplate["transactions"].GetArray();

  vtx.resize(transactions.Size() + 1);
  for (size_t i = 0, ie = transactions.Size(); i != ie; ++i) {
    rapidjson::Value &txSrc = transactions[i];
    BTC::Proto::Transaction &txDst = vtx[i + 1];

    // Get data
    if (!txSrc.HasMember("data") || !txSrc["data"].IsString())
      return false;

    const char *txData = txSrc["data"].GetString();
    size_t txDataSize = txSrc["data"].GetStringLength();
    buffer.reset();
    hexLowerCase2bin(txData, txDataSize, buffer.reserve(txDataSize/2));
    buffer.seekSet(0);
    BTC::Io<BTC::Proto::Transaction>::unserialize(buffer, txDst);
    if (buffer.eof())
      return false;

    // Get hash
    if (!txSrc.HasMember("hash") || !txSrc["hash"].IsString())
      return false;
    txDst.Hash.SetHex(txSrc["hash"].GetString());
  }

  return true;
}

bool buildCoinbaseFromTemplate(BTC::Proto::Transaction &coinbaseTx, BTC::Proto::AddressTy &address, const std::string &coinbaseMessage, rapidjson::Value &blockTemplate, size_t *extraNonceOffset)
{
  coinbaseTx.version = 1;

  // TxIn
  {
    coinbaseTx.txIn.resize(1);
    BTC::Proto::TxIn &txIn = coinbaseTx.txIn[0];
    txIn.previousOutputHash.SetNull();
    txIn.previousOutputIndex = std::numeric_limits<uint32_t>::max();

    // scriptsig
    if (!blockTemplate.HasMember("height") || !blockTemplate["height"].IsInt64())
      return false;

    xmstream scriptsig;
    // Height
    BTC::serializeForCoinbase(scriptsig, blockTemplate["height"].GetInt64());
    // Coinbase message
    scriptsig.write(coinbaseMessage.data(), coinbaseMessage.size());
    // Extra nonce
    *extraNonceOffset = scriptsig.offsetOf();
    scriptsig.write<uint64_t>(0);
    scriptsig.write<uint64_t>(0);
    xvectorFromStream(std::move(scriptsig), txIn.scriptSig);
    txIn.sequence = std::numeric_limits<uint32_t>::max();
  }

  // TxOut
  {
    coinbaseTx.txOut.resize(1);
    // Value
    BTC::Proto::TxOut &txOut = coinbaseTx.txOut[0];
    if (!blockTemplate.HasMember("coinbasevalue") || !blockTemplate["coinbasevalue"].IsInt64())
      return false;
    txOut.value = blockTemplate["coinbasevalue"].GetInt64();

    // pkScript (use single P2PKH)
    txOut.pkScript.resize(sizeof(BTC::Proto::AddressTy) + 5);
    xmstream p2pkh(txOut.pkScript.data(), txOut.pkScript.size());
    p2pkh.write<uint8_t>(BTC::Script::OP_DUP);
    p2pkh.write<uint8_t>(BTC::Script::OP_HASH160);
    p2pkh.write<uint8_t>(sizeof(BTC::Proto::AddressTy));
    p2pkh.write(address.begin(), address.size());
    p2pkh.write<uint8_t>(BTC::Script::OP_EQUALVERIFY);
    p2pkh.write<uint8_t>(BTC::Script::OP_CHECKSIG);
  }

  coinbaseTx.lockTime = 0;
  return true;
}

bool buildSegwitCoinbaseFromTemplate(BTC::Proto::Transaction &coinbaseTx, BTC::Proto::AddressTy &address, rapidjson::Value &blockTemplate)
{
  coinbaseTx.version = 2;

  // TxIn
  {
    coinbaseTx.txIn.resize(1);
    typename BTC::Proto::TxIn &txIn = coinbaseTx.txIn[0];
    txIn.previousOutputHash.SetNull();
    txIn.previousOutputIndex = std::numeric_limits<uint32_t>::max();

    // scriptsig
    if (!blockTemplate.HasMember("height") || !blockTemplate["height"].IsInt64())
      return false;

    xmstream scriptsig;
    // height
    BTC::serializeForCoinbase(scriptsig, blockTemplate["height"].GetInt64());
    // TODO: correct fill coinbase txin
    txIn.sequence = std::numeric_limits<uint32_t>::max();
  }

  // TxOut
  {
    coinbaseTx.txOut.resize(2);

    {
      // First txout (funds)
      BTC::Proto::TxOut &txOut = coinbaseTx.txOut[0];

      if (!blockTemplate.HasMember("coinbasevalue") || !blockTemplate["coinbasevalue"].IsInt64())
        return false;
      txOut.value = blockTemplate["coinbasevalue"].GetInt64();

      // pkScript (use single P2PKH)
      txOut.pkScript.resize(sizeof(BTC::Proto::AddressTy) + 5);
      xmstream p2pkh(txOut.pkScript.data(), txOut.pkScript.size());
      p2pkh.write<uint8_t>(BTC::Script::OP_DUP);
      p2pkh.write<uint8_t>(BTC::Script::OP_HASH160);
      p2pkh.write<uint8_t>(sizeof(BTC::Proto::AddressTy));
      p2pkh.write(address.begin(), address.size());
      p2pkh.write<uint8_t>(BTC::Script::OP_EQUALVERIFY);
      p2pkh.write<uint8_t>(BTC::Script::OP_CHECKSIG);

    }

    {
      // Second txout (witness)
      BTC::Proto::TxOut &txOut = coinbaseTx.txOut[1];

      if (!blockTemplate.HasMember("default_witness_commitment") || !blockTemplate["default_witness_commitment"].IsString())
        return false;
      const char *witnessCommitmentData = blockTemplate["default_witness_commitment"].GetString();
      size_t witnessCommitmentDataSize = blockTemplate["default_witness_commitment"].GetStringLength();
      txOut.value = 0;
      txOut.pkScript.resize(witnessCommitmentDataSize/2);
      hexLowerCase2bin(witnessCommitmentData, witnessCommitmentDataSize, txOut.pkScript.data());
    }
  }

  coinbaseTx.lockTime = 0;
  return true;
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
