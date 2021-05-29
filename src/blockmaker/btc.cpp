// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockmaker/btc.h"
#include "blockmaker/serializeJson.h"
#include "poolcommon/arith_uint256.h"
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

void Stratum::Notify::build(StratumWork *source, typename Proto::BlockHeader &header, uint32_t asicBoostData, CoinbaseTx &legacy, const std::vector<uint256> &merklePath, const MiningConfig &cfg, bool resetPreviousWork, xmstream &notifyMessage)
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
  if (msg.submit.MutableExtraNonce.size() != miningCfg.MutableExtraNonceSize)
    return false;
  if (workerCfg.AsicBoostEnabled && !msg.submit.VersionBits.has_value())
    return false;

  // Write target extra nonce to first txin
  {
    uint8_t *scriptSig = legacy.Data.data<uint8_t>() + legacy.ExtraNonceOffset;
    writeBinBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize, scriptSig);
    memcpy(scriptSig + miningCfg.FixedExtraNonceSize, msg.submit.MutableExtraNonce.data(), msg.submit.MutableExtraNonce.size());
  }
  {
    uint8_t *scriptSig = witness.Data.data<uint8_t>() + witness.ExtraNonceOffset;
    writeBinBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize, scriptSig);
    memcpy(scriptSig + miningCfg.FixedExtraNonceSize, msg.submit.MutableExtraNonce.data(), msg.submit.MutableExtraNonce.size());
  }

  // Calculate merkle root and build header
  header.hashMerkleRoot = calculateMerkleRoot(legacy.Data.data(), legacy.Data.sizeOf(), merklePath);
  header.nTime = msg.submit.Time;
  header.nNonce = msg.submit.Nonce;
  if (workerCfg.AsicBoostEnabled)
    header.nVersion = (jobVersion & ~workerCfg.VersionMask) | (msg.submit.VersionBits.value() & workerCfg.VersionMask);
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
