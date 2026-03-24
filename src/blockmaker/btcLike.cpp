#include "blockmaker/btcLike.h"
#include "blockmaker/merkleTree.h"

namespace BTC {
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
      result[i].TxId.setHexLE(txSrc["txid"].GetString());
      if (txSrc.HasMember("hash"))
        result[i].WitnessHash.setHexLE(txSrc["hash"].GetString());
    } else if (txSrc.HasMember("hash") && txSrc["hash"].IsString()) {
      result[i].TxId.setHexLE(txSrc["hash"].GetString());
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

bool calculateWitnessCommitment(rapidjson::Value &blockTemplate, xmstream &witnessCommitment, std::string &error)
{
  if (!blockTemplate.HasMember("default_witness_commitment") || !blockTemplate["default_witness_commitment"].IsString()) {
    error = "default_witness_commitment missing";
    return false;
  }

  const char *originalWitnessCommitment = blockTemplate["default_witness_commitment"].GetString();
  rapidjson::SizeType originalWitnessCommitmentSize = blockTemplate["default_witness_commitment"].GetStringLength();
  hex2bin(originalWitnessCommitment, originalWitnessCommitmentSize, witnessCommitment.reserve(originalWitnessCommitmentSize/2));
  return true;
}

void collectTransactions(const std::vector<TxData> &processedTransactions, xmstream &txHexData, std::vector<BaseBlob<256>> &merklePath, size_t &txNum)
{
  std::vector<BaseBlob<256>> txHashes;
  txHashes.emplace_back();
  txHashes.back().setNull();
  for (const auto &tx: processedTransactions) {
    txHexData.write(tx.HexData, tx.HexDataSize);
    txHashes.push_back(tx.TxId);
  }

  txNum = processedTransactions.size();

  // Build merkle path
  buildMerklePath(txHashes, 0, merklePath);
}

}
