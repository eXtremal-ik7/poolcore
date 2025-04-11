#include "blockmaker/btcLike.h"
#include "blockmaker/merkleTree.h"

namespace BTC {
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
      CCtxSha256 ctx;
      sha256Init(&ctx);
      sha256Update(&ctx, witnessMerkleRoot.begin(), witnessMerkleRoot.size());
      sha256Update(&ctx, defaultWitnessNonce, 32);
      sha256Final(&ctx, commitment.begin());
      sha256Init(&ctx);
      sha256Update(&ctx, commitment.begin(), commitment.size());
      sha256Final(&ctx, commitment.begin());
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
