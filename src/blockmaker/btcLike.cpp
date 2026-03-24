#include "blockmaker/btcLike.h"
#include "blockmaker/merkleTree.h"

namespace BTC {
bool transactionChecker(const std::vector<CBlockTemplateTx> &transactions, std::vector<TxData> &result)
{
  result.resize(transactions.size());
  for (size_t i = 0, ie = transactions.size(); i != ie; ++i) {
    const CBlockTemplateTx &txSrc = transactions[i];

    result[i].HexData = txSrc.Data.c_str();
    result[i].HexDataSize = txSrc.Data.size();

    if (txSrc.Txid.has_value()) {
      result[i].TxId = *txSrc.Txid;
      result[i].WitnessHash = txSrc.Hash;
    } else {
      result[i].TxId = txSrc.Hash;
    }
  }

  return true;
}

bool isSegwitEnabled(const std::vector<CBlockTemplateTx> &transactions)
{
  for (const auto &tx : transactions) {
    if (tx.Txid.has_value() && *tx.Txid != tx.Hash)
      return true;
  }

  return false;
}

bool calculateWitnessCommitment(const CBlockTemplateResult &blockTemplate, xmstream &witnessCommitment, std::string &error)
{
  if (!blockTemplate.Default_witness_commitment.has_value()) {
    error = "default_witness_commitment missing";
    return false;
  }

  const auto &commitment = *blockTemplate.Default_witness_commitment;
  witnessCommitment.write(commitment.data(), commitment.size());
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
