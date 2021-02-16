#include "blockmaker/doge.h"
#include "blockmaker/merkleTree.h"
#include "blockmaker/serializeJson.h"

static const unsigned char pchMergedMiningHeader[] = { 0xfa, 0xbe, 'm', 'm' };

static double getDifficulty(uint32_t bits)
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

static uint32_t getExpectedIndex(uint32_t nNonce, int nChainId, unsigned h)
{
  uint32_t rand = nNonce;
  rand = rand * 1103515245 + 12345;
  rand += nChainId;
  rand = rand * 1103515245 + 12345;

  return rand % (1 << h);
}

namespace DOGE {


double Stratum::Work::expectedWork(size_t index)
{
  return index == 0 ? LTC.expectedWork(0) : getDifficulty(DOGE.Header.nBits);
}

void Stratum::Work::updateLTCBlock()
{
  if (LTC.CBTxLegacy.Data.sizeOf() >= LTC.CBTxLegacy.ExtraDataOffset+sizeof(uint256)+4 &&
     LTC.CBTxWitness.Data.sizeOf() >= LTC.CBTxWitness.ExtraDataOffset+sizeof(uint256)+4) {
    // Update DOGE block header hash into LTC coinbase tx
    uint256 dogeHeaderHash = DOGE.Header.GetHash();
    std::reverse(dogeHeaderHash.begin(), dogeHeaderHash.end());
    memcpy(LTC.CBTxLegacy.Data.data<uint8_t>() + LTC.CBTxLegacy.ExtraDataOffset+4, dogeHeaderHash.begin(), dogeHeaderHash.size());
    memcpy(LTC.CBTxWitness.Data.data<uint8_t>() + LTC.CBTxWitness.ExtraDataOffset+4, dogeHeaderHash.begin(), dogeHeaderHash.size());
  }
}

void Stratum::Work::updateDOGEBlock()
{
  uint32_t chainId = DOGE.Header.nVersion >> 16;
  LTC.CBTxWitness.Data.seekSet(0);
  BTC::unserialize(LTC.CBTxWitness.Data, DOGE.Header.ParentBlockCoinbaseTx);

  DOGE.Header.HashBlock.SetNull();

  DOGE.Header.MerkleBranch.resize(LTC.MerklePath.size());
  for (size_t i = 0, ie = LTC.MerklePath.size(); i != ie; ++i)
    DOGE.Header.MerkleBranch[i] = LTC.MerklePath[i];

  DOGE.Header.Index = 0;
  DOGE.Header.ChainMerkleBranch.resize(0);
  DOGE.Header.ChainIndex = getExpectedIndex(0, chainId, 0);
  DOGE.Header.ParentBlock = LTC.Header;
}

struct TxData {
  const char *HexData;
  size_t HexDataSize;
  uint256 TxId;
  uint256 WitnessHash;
};

static bool transactionChecker(rapidjson::Value::Array transactions, std::vector<TxData> &result)
{
  result.resize(transactions.Size());
  for (size_t i = 0, ie = transactions.Size(); i != ie; ++i) {
    rapidjson::Value &txSrc = transactions[i];
    if (!txSrc.HasMember("data") || !txSrc["data"].IsString())
      return false;
    if (!txSrc.HasMember("txid") || !txSrc["txid"].IsString())
      return false;

    result[i].HexData = txSrc["data"].GetString();
    result[i].HexDataSize = txSrc["data"].GetStringLength();
    result[i].TxId.SetHex(txSrc["txid"].GetString());
    if (txSrc.HasMember("hash"))
      result[i].WitnessHash.SetHex(txSrc["hash"].GetString());
  }

  return true;
}

bool Stratum::Work::loadFromTemplate(rapidjson::Value &document, uint64_t uniqueWorkId, const MiningConfig &cfg, PoolBackend *backend, const std::string &ticker, Proto::AddressTy &miningAddress, const void *coinBaseExtraData, size_t coinbaseExtraSize, std::string &error, uint32_t txNumLimit)
{
  if (ticker == "DOGE" || ticker == "DOGE.testnet" || ticker == "DOGE.regtest") {
    DOGE.Height = 0;
    DOGE.UniqueWorkId = uniqueWorkId;
    DOGE.TxData.reset();
    DOGE.Backend = backend;
    xmstream witnessCommitment;

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
    bool segwitEnabled = false;
    std::vector<TxData> processedTransactions;

    // Check segwit enabled (compare txid and hash for all transactions)
    for (rapidjson::SizeType i = 0, ie = transactions.Size(); i != ie; ++i) {
      rapidjson::Value &tx = transactions[i];
      if (tx.HasMember("txid") && tx["txid"].IsString() &&
          tx.HasMember("hash") && tx["hash"].IsString()) {
        rapidjson::Value &txid = tx["txid"];
        rapidjson::Value &hash = tx["hash"];
        if (txid.GetStringLength() == hash.GetStringLength()) {
          if (memcmp(txid.GetString(), hash.GetString(), txid.GetStringLength()) != 0) {
            segwitEnabled = true;
            break;
          }
        }
      }
    }

    transactionChecker(transactions, processedTransactions);

    if (segwitEnabled) {
      if (!blockTemplate.HasMember("default_witness_commitment") || !blockTemplate["default_witness_commitment"].IsString()) {
        error = "default_witness_commitment missing";
        return false;
      }

      const char *originalWitnessCommitment = blockTemplate["default_witness_commitment"].GetString();
      rapidjson::SizeType originalWitnessCommitmentSize = blockTemplate["default_witness_commitment"].GetStringLength();
      hex2bin(originalWitnessCommitment, originalWitnessCommitmentSize, witnessCommitment.reserve(originalWitnessCommitmentSize/2));
    }

    DOGE.Height = height.GetUint64();
    Proto::BlockHeader &header = DOGE.Header;
    header.nVersion = version.GetUint() | DOGE::Proto::BlockHeader::VERSION_AUXPOW;
    header.hashPrevBlock.SetHex(hashPrevBlock.GetString());
    header.hashMerkleRoot.SetNull();
    header.nTime = curtime.GetUint();
    header.nBits = strtoul(bits.GetString(), nullptr, 16);
    header.nNonce = 0;

    // Serialize header and transactions count
    {
      xmstream stream;
      BTC::serializeVarSize(stream, processedTransactions.size() + 1);
      bin2hexLowerCase(stream.data(), DOGE.TxData.reserve<char>(stream.sizeOf()*2), stream.sizeOf());
    }

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
      BTC::serializeForCoinbase(scriptsig, DOGE.Height);
      // Coinbase message
      scriptsig.write(coinBaseExtraData, coinbaseExtraSize);
      // DOGE does not have extra nonce
      xvectorFromStream(std::move(scriptsig), txIn.scriptSig);
      txIn.sequence = std::numeric_limits<uint32_t>::max();
    }

    // TxOut
    {
      BTC::Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
      DOGE.BlockReward = txOut.value = blockReward;

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
      BTC::Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
      txOut.value = 0;
      txOut.pkScript.resize(witnessCommitment.sizeOf());
      memcpy(txOut.pkScript.data(), witnessCommitment.data(), witnessCommitment.sizeOf());
    }

    coinbaseTx.lockTime = 0;

    xmstream cbTxLegacy;
    xmstream cbTxWitness;
    BTC::Io<BTC::Proto::Transaction>::serialize(cbTxLegacy, coinbaseTx, false);
    BTC::Io<BTC::Proto::Transaction>::serialize(cbTxWitness, coinbaseTx, true);
    bin2hexLowerCase(cbTxWitness.data(), DOGE.TxData.reserve<char>(cbTxWitness.sizeOf()*2), cbTxWitness.sizeOf());

    // Transactions
    std::vector<uint256> txHashes;
    txHashes.emplace_back();
    txHashes.back().SetNull();
    for (const auto &tx: processedTransactions) {
      DOGE.TxData.write(tx.HexData, tx.HexDataSize);
      txHashes.push_back(tx.TxId);
    }

    // Merkle root
    std::vector<uint256> merklePath;
    dumpMerkleTree(txHashes, merklePath);
    DOGE.Header.hashMerkleRoot = calculateMerkleRoot(cbTxLegacy.data(), cbTxLegacy.sizeOf(), merklePath);

    DOGE.TxNum = processedTransactions.size();
    updateLTCBlock();
    updateDOGEBlock();
    return true;
  } else {
    // Build coinbase message
    // <Merged mining signature> <chain merkle root> <chain merkle tree size> <extra nonce fixed part>
    uint8_t buffer[1024];
    xmstream coinbaseMsg(buffer, sizeof(buffer));
    coinbaseMsg.reset();
    // DOGE merged mining signature
    coinbaseMsg.write(pchMergedMiningHeader, sizeof(pchMergedMiningHeader));
    // chain merkle root, we use DOGE block header hash only
    {
      uint256 hash = DOGE.Header.GetHash();
      std::reverse(hash.begin(), hash.end());
      coinbaseMsg.write(hash.begin(), sizeof(uint256));
    }
    // chain merkle tree size, we use 1 alltimes
    coinbaseMsg.write<uint32_t>(1);
    // extra nonce fixed part, we use 0 alltimes
    coinbaseMsg.write<uint32_t>(0);

    // Load LTC template
    if (LTC.loadFromTemplate(document, uniqueWorkId, cfg, backend, ticker, miningAddress, coinbaseMsg.data(), coinbaseMsg.sizeOf(), error)) {
      updateDOGEBlock();
      return true;
    } else {
      return false;
    }
  }
}

bool Stratum::Work::prepareForSubmit(const WorkerConfig &workerCfg, const MiningConfig &miningCfg, const StratumMessage &msg)
{
  if (!LTC.prepareForSubmit(workerCfg, miningCfg, msg))
    return false;

  updateDOGEBlock();

  // Build DOGE block
  {
    uint8_t buffer[4096];
    xmstream bin(buffer, sizeof(buffer));
    bin.reset();
    BTC::serialize(bin, DOGE.Header);

    DOGE.PreparedBlockData.reset();
    bin2hexLowerCase(bin.data(), DOGE.PreparedBlockData.reserve<char>(bin.sizeOf()*2), bin.sizeOf());

    DOGE.PreparedBlockData.write(DOGE.TxData.data(), DOGE.TxData.sizeOf());
  }

  return true;
}

}

void BTC::Io<DOGE::Proto::BlockHeader>::serialize(xmstream &dst, const DOGE::Proto::BlockHeader &data)
{
  BTC::serialize(dst, *(DOGE::Proto::PureBlockHeader*)&data);
  if (data.nVersion & DOGE::Proto::BlockHeader::VERSION_AUXPOW) {
    BTC::serialize(dst, data.ParentBlockCoinbaseTx);

    BTC::serialize(dst, data.HashBlock);
    BTC::serialize(dst, data.MerkleBranch);
    BTC::serialize(dst, data.Index);

    BTC::serialize(dst, data.ChainMerkleBranch);
    BTC::serialize(dst, data.ChainIndex);
    BTC::serialize(dst, data.ParentBlock);
  }
}

void serializeJsonInside(xmstream &stream, const DOGE::Proto::BlockHeader &header)
{
  serializeJson(stream, "version", header.nVersion); stream.write(',');
  serializeJson(stream, "hashPrevBlock", header.hashPrevBlock); stream.write(',');
  serializeJson(stream, "hashMerkleRoot", header.hashMerkleRoot); stream.write(',');
  serializeJson(stream, "time", header.nTime); stream.write(',');
  serializeJson(stream, "bits", header.nBits); stream.write(',');
  serializeJson(stream, "nonce", header.nNonce); stream.write(',');
  serializeJson(stream, "parentBlockCoinbaseTx", header.ParentBlockCoinbaseTx); stream.write(',');
  serializeJson(stream, "hashBlock", header.HashBlock); stream.write(',');
  serializeJson(stream, "merkleBranch", header.MerkleBranch); stream.write(',');
  serializeJson(stream, "index", header.Index); stream.write(',');
  serializeJson(stream, "chainMerkleBranch", header.ChainMerkleBranch); stream.write(',');
  serializeJson(stream, "chainIndex", header.ChainIndex); stream.write(',');
  stream.write("\"parentBlock\":{");
  serializeJsonInside(stream, header.ParentBlock);
  stream.write('}');
}
