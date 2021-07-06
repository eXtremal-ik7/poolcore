#include "blockmaker/btcLike.h"
#include <openssl/sha.h>
#include "poolinstances/stratumMsg.h"

namespace BTC {
EStratumDecodeStatusTy StratumMessage::decodeStratumMessage(const char *in, size_t size)
{
  rapidjson::Document document;
  document.Parse(in, size);
  if (document.HasParseError()) {
    return EStratumStatusJsonError;
  }

  if (!(document.HasMember("id") && document.HasMember("method") && document.HasMember("params")))
    return EStratumStatusFormatError;

  // Some clients put null to 'params' field
  if (document["params"].IsNull())
    document["params"].SetArray();

  if (!(document["method"].IsString() && document["params"].IsArray()))
    return EStratumStatusFormatError;

  if (document["id"].IsUint64())
    IntegerId = document["id"].GetUint64();
  else if (document["id"].IsString())
    StringId = document["id"].GetString();
  else
    return EStratumStatusFormatError;

  std::string method = document["method"].GetString();
  const rapidjson::Value::Array &params = document["params"].GetArray();
  if (method == "mining.subscribe") {
    Method = ESubscribe;
    if (params.Size() >= 1) {
      if (params[0].IsString())
        Subscribe.minerUserAgent = params[0].GetString();
    }

    if (params.Size() >= 2) {
      if (params[1].IsString())
        Subscribe.sessionId = params[1].GetString();
    }

    if (params.Size() >= 3) {
      if (params[2].IsString())
        Subscribe.connectHost = params[2].GetString();
    }

    if (params.Size() >= 4) {
      if (params[3].IsUint())
        Subscribe.connectPort = params[3].GetUint();
    }
  } else if (method == "mining.authorize" && params.Size() >= 2) {
    Method = EAuthorize;
    if (params[0].IsString() && params[1].IsString()) {
      Authorize.login = params[0].GetString();
      Authorize.password = params[1].GetString();
    } else {
      return EStratumStatusFormatError;
    }
  } else if (method == "mining.extranonce.subscribe") {
    Method = EExtraNonceSubscribe;
  } else if (method == "mining.submit" && params.Size() >= 5) {
    if (params[0].IsString() &&
        params[1].IsString() &&
        params[2].IsString() &&
        params[3].IsString() && params[3].GetStringLength() == 8 &&
        params[4].IsString() && params[4].GetStringLength() == 8) {
      Method = ESubmit;
      Submit.WorkerName = params[0].GetString();
      Submit.JobId = params[1].GetString();
      {
        // extra nonce mutable part
        Submit.MutableExtraNonce.resize(params[2].GetStringLength() / 2);
        hex2bin(params[2].GetString(), params[2].GetStringLength(), Submit.MutableExtraNonce.data());
      }
      Submit.Time = readHexBE<uint32_t>(params[3].GetString(), 4);
      Submit.Nonce = readHexBE<uint32_t>(params[4].GetString(), 4);

      if (params.Size() >= 6 && params[5].IsString())
        Submit.VersionBits = readHexBE<uint32_t>(params[5].GetString(), 4);
    } else {
      return EStratumStatusFormatError;
    }
  } else if (method == "mining.multi_version" && params.Size() >= 1) {
    Method = EMultiVersion;
    if (params[0].IsUint()) {
      MultiVersion.Version = params[0].GetUint();
    } else {
      return EStratumStatusFormatError;
    }
  } else if (method == "mining.configure" && params.Size() >= 2) {
    Method = EMiningConfigure;
    MiningConfigure.ExtensionsField = 0;
    // Example:
    // {
    //    "id":1,
    //    "method":"mining.configure",
    //    "params":[
    //       [
    //          "version-rolling"
    //       ],
    //       {
    //          "version-rolling.min-bit-count":2,
    //          "version-rolling.mask":"00c00000"
    //       }
    //    ]
    // }
    if (params[0].IsArray() &&
        params[1].IsObject()) {
      rapidjson::Value::Array extensions = params[0].GetArray();
      rapidjson::Value &arguments = params[1];
      for (rapidjson::SizeType i = 0, ie = extensions.Size(); i != ie; ++i) {
        if (strcmp(extensions[i].GetString(), "version-rolling") == 0) {
          MiningConfigure.ExtensionsField |= StratumMiningConfigure::EVersionRolling;
          if (arguments.HasMember("version-rolling.mask") && arguments["version-rolling.mask"].IsString())
            MiningConfigure.VersionRollingMask = readHexBE<uint32_t>(arguments["version-rolling.mask"].GetString(), 4);
          if (arguments.HasMember("version-rolling.min-bit-count") && arguments["version-rolling.min-bit-count"].IsUint())
            MiningConfigure.VersionRollingMinBitCount = arguments["version-rolling.min-bit-count"].GetUint();
        } else if (strcmp(extensions[i].GetString(), "minimum-difficulty") == 0) {
          if (arguments.HasMember("minimum-difficulty.value")) {
            if (arguments["minimum-difficulty.value"].IsUint64())
              MiningConfigure.MinimumDifficultyValue = static_cast<double>(arguments["minimum-difficulty.value"].GetUint64());
            else if (arguments["minimum-difficulty.value"].IsDouble())
              MiningConfigure.MinimumDifficultyValue = arguments["minimum-difficulty.value"].GetDouble();
          }
          MiningConfigure.ExtensionsField |= StratumMiningConfigure::EMinimumDifficulty;
        } else if (strcmp(extensions[i].GetString(), "subscribe-extranonce") == 0) {
          MiningConfigure.ExtensionsField |= StratumMiningConfigure::ESubscribeExtraNonce;
        }
      }
    } else {
      return EStratumStatusFormatError;
    }
  } else if (method == "mining.suggest_difficulty" && params.Size() >= 1) {
    Method = EMiningSuggestDifficulty;
    if (params[0].IsDouble()) {
      MiningSuggestDifficulty.Difficulty = params[0].GetDouble();
    } else if (params[0].IsUint64()) {
      MiningSuggestDifficulty.Difficulty = static_cast<double>(params[0].GetUint64());
    } else {
      return EStratumStatusFormatError;
    }
  } else {
    return EStratumStatusFormatError;
  }

  return EStratumStatusOk;
}


bool addTransaction(TxTree *tree, size_t index, size_t txNumLimit, std::vector<TxData> &result, int64_t *blockReward)
{
  // TODO: keep transactions depend on other transactions in same block
  if (tree[index].Visited || tree[index].DependsOn != std::numeric_limits<size_t>::max())
    return true;
  if (result.size() >= txNumLimit)
    return false;

  result.push_back(tree[index].Data);
  tree[index].Visited = true;
  *blockReward += tree[index].Fee;
  return true;
}

bool transactionChecker(rapidjson::Value::Array transactions, std::vector<TxData> &result)
{
  result.resize(transactions.Size());
  for (size_t i = 0, ie = transactions.Size(); i != ie; ++i) {
    rapidjson::Value &txSrc = transactions[i];

    if (!txSrc.HasMember("data") || !txSrc["data"].IsString())
      return false;
    result[i].HexData = txSrc["data"].GetString();
    result[i].HexDataSize = txSrc["data"].GetStringLength();

    if (txSrc.HasMember("txid") && txSrc["txid"].IsString()) {
      result[i].TxId.SetHex(txSrc["txid"].GetString());
      if (txSrc.HasMember("hash"))
        result[i].WitnessHash.SetHex(txSrc["hash"].GetString());
    } else if (txSrc.HasMember("hash") && txSrc["hash"].IsString()) {
      result[i].TxId.SetHex(txSrc["hash"].GetString());
    } else {
      return false;
    }
  }

  return true;
}

bool isSegwitEnabled(rapidjson::Value::Array transactions)
{
  for (rapidjson::SizeType i = 0, ie = transactions.Size(); i != ie; ++i) {
    rapidjson::Value &tx = transactions[i];
    if (tx.HasMember("txid") && tx["txid"].IsString() &&
        tx.HasMember("hash") && tx["hash"].IsString()) {
      rapidjson::Value &txid = tx["txid"];
      rapidjson::Value &hash = tx["hash"];
      if (txid.GetStringLength() == hash.GetStringLength()) {
        if (memcmp(txid.GetString(), hash.GetString(), txid.GetStringLength()) != 0) {
          return true;
        }
      }
    }
  }

  return false;
}

bool calculateWitnessCommitment(rapidjson::Value &blockTemplate, bool txFilter, std::vector<TxData> &processedTransactions, xmstream &witnessCommitment, std::string &error)
{
  if (!blockTemplate.HasMember("default_witness_commitment") || !blockTemplate["default_witness_commitment"].IsString()) {
    error = "default_witness_commitment missing";
    return false;
  }

  const char *originalWitnessCommitment = blockTemplate["default_witness_commitment"].GetString();
  rapidjson::SizeType originalWitnessCommitmentSize = blockTemplate["default_witness_commitment"].GetStringLength();

  if (!txFilter) {
    hex2bin(originalWitnessCommitment, originalWitnessCommitmentSize, witnessCommitment.reserve(originalWitnessCommitmentSize/2));
  } else {
    // Collect witness hashes to array
    std::vector<uint256> witnessHashes;
    witnessHashes.emplace_back();
    witnessHashes.back().SetNull();
    for (const auto &tx: processedTransactions)
      witnessHashes.push_back(tx.WitnessHash);

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

  return true;
}

void collectTransactions(const std::vector<TxData> &processedTransactions, xmstream &txHexData, std::vector<uint256> &merklePath, size_t &txNum)
{
  std::vector<uint256> txHashes;
  txHashes.emplace_back();
  txHashes.back().SetNull();
  for (const auto &tx: processedTransactions) {
    txHexData.write(tx.HexData, tx.HexDataSize);
    txHashes.push_back(tx.TxId);
  }

  txNum = processedTransactions.size();

  // Build merkle path
  dumpMerkleTree(txHashes, merklePath);
}

}
