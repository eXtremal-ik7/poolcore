#include "blockmaker/zec.h"

void BTC::Io<ZEC::Proto::BlockHeader>::serialize(xmstream &dst, const ZEC::Proto::BlockHeader &data)
{
  BTC::serialize(dst, data.nVersion);
  BTC::serialize(dst, data.hashPrevBlock);
  BTC::serialize(dst, data.hashMerkleRoot);
  BTC::serialize(dst, data.hashLightClientRoot);
  BTC::serialize(dst, data.nTime);
  BTC::serialize(dst, data.nBits);
  BTC::serialize(dst, data.nNonce);
  BTC::serialize(dst, data.nSolution);
}

void BTC::Io<ZEC::Proto::CompressedG1>::serialize(xmstream &dst, const ZEC::Proto::CompressedG1 &data)
{
  uint8_t leadingByte = ZEC::Proto::G1_PREFIX_MASK;
  if (data.y_lsb)
    leadingByte |= 1;
  BTC::serialize(dst, leadingByte);
  BTC::serialize(dst, data.x);
}

void BTC::Io<ZEC::Proto::CompressedG1>::unserialize(xmstream &src, ZEC::Proto::CompressedG1 &data)
{
  uint8_t leadingByte;
  BTC::unserialize(src, leadingByte);
  if ((leadingByte & (~1)) != ZEC::Proto::G1_PREFIX_MASK) {
    src.seekEnd(0, true);
    return;
  }

  data.y_lsb = leadingByte & 1;
  BTC::unserialize(src, data.x);
}

void BTC::Io<ZEC::Proto::CompressedG2>::serialize(xmstream &dst, const ZEC::Proto::CompressedG2 &data)
{
  uint8_t leadingByte = ZEC::Proto::G2_PREFIX_MASK;
  if (data.y_gt)
    leadingByte |= 1;
  BTC::serialize(dst, leadingByte);
  BTC::serialize(dst, data.x);
}


void BTC::Io<ZEC::Proto::CompressedG2>::unserialize(xmstream &src, ZEC::Proto::CompressedG2 &data)
{
  uint8_t leadingByte;
  BTC::unserialize(src, leadingByte);
  if ((leadingByte & (~1)) != ZEC::Proto::G2_PREFIX_MASK) {
    src.seekEnd(0, true);
    return;
  }

  data.y_gt = leadingByte & 1;
  BTC::unserialize(src, data.x);
}

void BTC::Io<ZEC::Proto::SpendDescription>::serialize(xmstream &dst, const ZEC::Proto::SpendDescription &data)
{
  BTC::serialize(dst, data.cv);
  BTC::serialize(dst, data.anchor);
  BTC::serialize(dst, data.nullifer);
  BTC::serialize(dst, data.rk);
  BTC::serialize(dst, data.zkproof);
  BTC::serialize(dst, data.spendAuthSig);
}

void BTC::Io<ZEC::Proto::SpendDescription>::unserialize(xmstream &src, ZEC::Proto::SpendDescription &data)
{
  BTC::unserialize(src, data.cv);
  BTC::unserialize(src, data.anchor);
  BTC::unserialize(src, data.nullifer);
  BTC::unserialize(src, data.rk);
  BTC::unserialize(src, data.zkproof);
  BTC::unserialize(src, data.spendAuthSig);
}

void BTC::Io<ZEC::Proto::OutputDescription>::serialize(xmstream &dst, const ZEC::Proto::OutputDescription &data)
{
  BTC::serialize(dst, data.cv);
  BTC::serialize(dst, data.cmu);
  BTC::serialize(dst, data.ephemeralKey);
  BTC::serialize(dst, data.encCiphertext);
  BTC::serialize(dst, data.outCiphertext);
  BTC::serialize(dst, data.zkproof);
}

void BTC::Io<ZEC::Proto::OutputDescription>::unserialize(xmstream &src, ZEC::Proto::OutputDescription &data)
{
  BTC::unserialize(src, data.cv);
  BTC::unserialize(src, data.cmu);
  BTC::unserialize(src, data.ephemeralKey);
  BTC::unserialize(src, data.encCiphertext);
  BTC::unserialize(src, data.outCiphertext);
  BTC::unserialize(src, data.zkproof);
}

void BTC::Io<ZEC::Proto::PHGRProof>::serialize(xmstream &dst, const ZEC::Proto::PHGRProof &data)
{
  BTC::serialize(dst, data.g_A);
  BTC::serialize(dst, data.g_A_prime);
  BTC::serialize(dst, data.g_B);
  BTC::serialize(dst, data.g_B_prime);
  BTC::serialize(dst, data.g_C);
  BTC::serialize(dst, data.g_C_prime);
  BTC::serialize(dst, data.g_K);
  BTC::serialize(dst, data.g_H);
}

void BTC::Io<ZEC::Proto::PHGRProof>::unserialize(xmstream &src, ZEC::Proto::PHGRProof &data)
{
  BTC::unserialize(src, data.g_A);
  BTC::unserialize(src, data.g_A_prime);
  BTC::unserialize(src, data.g_B);
  BTC::unserialize(src, data.g_B_prime);
  BTC::unserialize(src, data.g_C);
  BTC::unserialize(src, data.g_C_prime);
  BTC::unserialize(src, data.g_K);
  BTC::unserialize(src, data.g_H);
}

void BTC::Io<ZEC::Proto::JSDescription, bool>::serialize(xmstream &dst, const ZEC::Proto::JSDescription &data, bool useGroth)
{
  BTC::serialize(dst, data.vpub_old);
  BTC::serialize(dst, data.vpub_new);
  BTC::serialize(dst, data.anchor);
  BTC::serialize(dst, data.nullifier1); BTC::serialize(dst, data.nullifier2);
  BTC::serialize(dst, data.commitment1); BTC::serialize(dst, data.commitment2);
  BTC::serialize(dst, data.ephemeralKey);
  BTC::serialize(dst, data.randomSeed);
  BTC::serialize(dst, data.mac1); BTC::serialize(dst, data.mac2);
  if (useGroth)
    BTC::serialize(dst, data.zkproof);
  else
    BTC::serialize(dst, data.phgrProof);

  BTC::serialize(dst, data.ciphertext1);
  BTC::serialize(dst, data.ciphertext2);
}

void BTC::Io<ZEC::Proto::JSDescription, bool>::unserialize(xmstream &src, ZEC::Proto::JSDescription &data, bool useGroth)
{
  BTC::unserialize(src, data.vpub_old);
  BTC::unserialize(src, data.vpub_new);
  BTC::unserialize(src, data.anchor);
  BTC::unserialize(src, data.nullifier1); BTC::unserialize(src, data.nullifier2);
  BTC::unserialize(src, data.commitment1); BTC::unserialize(src, data.commitment2);
  BTC::unserialize(src, data.ephemeralKey);
  BTC::unserialize(src, data.randomSeed);
  BTC::unserialize(src, data.mac1); BTC::unserialize(src, data.mac2);
  if (useGroth)
    BTC::unserialize(src, data.zkproof);
  else
    BTC::unserialize(src, data.phgrProof);

  BTC::unserialize(src, data.ciphertext1);
  BTC::unserialize(src, data.ciphertext2);
}

void BTC::Io<ZEC::Proto::Transaction>::serialize(xmstream &dst, const ZEC::Proto::Transaction &data)
{
  uint32_t header = (static_cast<int32_t>(data.fOverwintered) << 31) | data.version;
  BTC::serialize(dst, header);

  if (data.fOverwintered)
    BTC::serialize(dst, data.nVersionGroupId);

  bool isOverwinterV3 =
      data.fOverwintered &&
      data.nVersionGroupId == ZEC::Proto::OVERWINTER_VERSION_GROUP_ID &&
      data.version == ZEC::Proto::OVERWINTER_TX_VERSION;
  bool isSaplingV4 =
      data.fOverwintered &&
      data.nVersionGroupId == ZEC::Proto::SAPLING_VERSION_GROUP_ID &&
      data.version == ZEC::Proto::SAPLING_TX_VERSION;
  bool useGroth = data.fOverwintered && data.version >= ZEC::Proto::SAPLING_TX_VERSION;

  BTC::serialize(dst, data.txIn);
  BTC::serialize(dst, data.txOut);
  BTC::serialize(dst, data.lockTime);

  if (isOverwinterV3 || isSaplingV4)
    BTC::serialize(dst, data.nExpiryHeight);
  if (isSaplingV4) {
    BTC::serialize(dst, data.valueBalance);
    BTC::serialize(dst, data.vShieldedSpend);
    BTC::serialize(dst, data.vShieldedOutput);
  }
  if (data.version >= 2) {
    BTC::Io<decltype (data.vJoinSplit), bool>::serialize(dst, data.vJoinSplit, useGroth);
    if (!data.vJoinSplit.empty()) {
      BTC::serialize(dst, data.joinSplitPubKey);
      BTC::serialize(dst, data.joinSplitSig);
    }
  }
  if (isSaplingV4 && !(data.vShieldedSpend.empty() && data.vShieldedOutput.empty()))
    BTC::serialize(dst, data.bindingSig);
}

void BTC::Io<ZEC::Proto::Transaction>::unserialize(xmstream &src, ZEC::Proto::Transaction &data)
{
  uint32_t header;
  BTC::unserialize(src, header);
  data.fOverwintered = header >> 31;
  data.version = header & 0x7FFFFFFF;

  if (data.fOverwintered)
    BTC::unserialize(src, data.nVersionGroupId);

  bool isOverwinterV3 =
      data.fOverwintered &&
      data.nVersionGroupId == ZEC::Proto::OVERWINTER_VERSION_GROUP_ID &&
      data.version == ZEC::Proto::OVERWINTER_TX_VERSION;
  bool isSaplingV4 =
      data.fOverwintered &&
      data.nVersionGroupId == ZEC::Proto::SAPLING_VERSION_GROUP_ID &&
      data.version == ZEC::Proto::SAPLING_TX_VERSION;
  bool useGroth = data.fOverwintered && data.version >= ZEC::Proto::SAPLING_TX_VERSION;

  if (data.fOverwintered && !(isOverwinterV3 || isSaplingV4)) {
    src.seekEnd(0, true);
    return;
  }

  BTC::unserialize(src, data.txIn);
  BTC::unserialize(src, data.txOut);
  BTC::unserialize(src, data.lockTime);

  if (isOverwinterV3 || isSaplingV4)
    BTC::unserialize(src, data.nExpiryHeight);
  if (isSaplingV4) {
    BTC::unserialize(src, data.valueBalance);
    BTC::unserialize(src, data.vShieldedSpend);
    BTC::unserialize(src, data.vShieldedOutput);
  }
  if (data.version >= 2) {
    BTC::Io<decltype (data.vJoinSplit), bool>::unserialize(src, data.vJoinSplit, useGroth);
    if (!data.vJoinSplit.empty()) {
      BTC::unserialize(src, data.joinSplitPubKey);
      BTC::unserialize(src, data.joinSplitSig);
    }
  }
  if (isSaplingV4 && !(data.vShieldedSpend.empty() && data.vShieldedOutput.empty()))
    BTC::unserialize(src, data.bindingSig);
}


namespace ZEC {

bool Proto::checkConsensus(const ZEC::Proto::BlockHeader&, CheckConsensusCtx&, ZEC::Proto::ChainParams&, double *shareDiff)
{
  // TODO: implement
  *shareDiff = 0;
  return false;
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
      !blockTemplate.HasMember("finalsaplingroothash") ||
      !blockTemplate.HasMember("curtime") ||
      !blockTemplate.HasMember("bits")) {
    return false;
  }

  rapidjson::Value &version = blockTemplate["version"];
  rapidjson::Value &hashPrevBlock = blockTemplate["previousblockhash"];
  rapidjson::Value &finalSaplingRootHash = blockTemplate["finalsaplingroothash"];
  rapidjson::Value &curtime = blockTemplate["curtime"];
  rapidjson::Value &bits = blockTemplate["bits"];

  header.nVersion = version.GetUint();
  header.hashPrevBlock.SetHex(hashPrevBlock.GetString());
  header.hashLightClientRoot.SetHex(finalSaplingRootHash.GetString());
  header.hashMerkleRoot.SetNull();
  header.nTime = curtime.GetUint();
  header.nBits = strtoul(bits.GetString(), nullptr, 16);
  header.nNonce.SetNull();
  header.nSolution.resize(0);
  *jobVersion = header.nVersion;
  return true;
}

bool Stratum::CoinbaseBuilder::prepare(int64_t *blockReward,
                                       int64_t *devFee,
                                       xmstream &devScriptPubKey,
                                       rapidjson::Value &blockTemplate)
{
  if (!blockTemplate.HasMember("coinbasetxn") || !blockTemplate["coinbasetxn"].IsObject())
    return false;

  rapidjson::Value &coinbasetxn = blockTemplate["coinbasetxn"];
  if (!coinbasetxn.HasMember("data") || !coinbasetxn["data"].IsString())
    return false;

  rapidjson::Value &data = coinbasetxn["data"];
  uint8_t buffer[1024];
  xmstream stream(buffer, sizeof(buffer));
  stream.reset();
  hex2bin(data.GetString(), data.GetStringLength(), stream.reserve(data.GetStringLength()/2));

  Proto::Transaction tx;
  stream.seekSet(0);
  BTC::unserialize(stream, tx);
  if (stream.eof())
    return false;

  if (tx.txOut.size() == 1) {
    *blockReward = tx.txOut[0].value;
    *devFee = 0;
  } else if (tx.txOut.size() == 2) {
    *blockReward = tx.txOut[0].value;
    *devFee = tx.txOut[1].value;
    devScriptPubKey.write(tx.txOut[1].pkScript.data(), tx.txOut[1].pkScript.size());
  } else {
    return false;
  }

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
                                     bool,
                                     const xmstream&,
                                     BTC::CoinbaseTx &legacy,
                                     BTC::CoinbaseTx &witness)
{
  Proto::Transaction coinbaseTx;

  coinbaseTx.version = 1;
  coinbaseTx.fOverwintered = false;

  // TxIn
  {
    coinbaseTx.txIn.resize(1);
    typename Proto::TxIn &txIn = coinbaseTx.txIn[0];
    txIn.previousOutputHash.SetNull();
    txIn.previousOutputIndex = std::numeric_limits<uint32_t>::max();

    // scriptsig
    xmstream scriptsig;
    // Height
    BTC::serializeForCoinbase(scriptsig, height);
    // Coinbase extra data
    if (coinbaseData)
      scriptsig.write(coinbaseData, coinbaseSize);
    // Coinbase message
    scriptsig.write(coinbaseMessage.data(), coinbaseMessage.size());

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

  coinbaseTx.lockTime = 0;
  BTC::Io<typename Proto::Transaction>::serialize(legacy.Data, coinbaseTx);
  BTC::Io<typename Proto::Transaction>::serialize(witness.Data, coinbaseTx);
}

void Stratum::Notify::build(CWork *source, typename Proto::BlockHeader &header, uint32_t asicBoostData, BTC::CoinbaseTx &legacy, const std::vector<uint256> &merklePath, const MiningConfig &cfg, bool resetPreviousWork, xmstream &notifyMessage)
{
  // {"id": null, "method": "mining.notify", "params": ["JOB_ID", "VERSION", "PREVHASH", "MERKLEROOT", "RESERVED", "TIME", "BITS", CLEAN_JOBS]}

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

      {
        // Version
        params.addString(writeHexBE(header.nVersion, sizeof(header.nVersion)));
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


      // Merkle root
      {
        std::string hash;
        const uint32_t *data = reinterpret_cast<const uint32_t*>(header.hashMerkleRoot.begin());
        for (unsigned i = 0; i < 8; i++) {
          hash.append(writeHexBE(data[i], 4));
        }
        params.addString(hash);
      }

      // light client root
      {
        std::string hash;
        const uint32_t *data = reinterpret_cast<const uint32_t*>(header.hashLightClientRoot.begin());
        for (unsigned i = 0; i < 8; i++) {
          hash.append(writeHexBE(data[i], 4));
        }
        params.addString(hash);
      }
      // nTime
      params.addString(writeHexBE(header.nTime, sizeof(header.nTime)));
      // nBits
      params.addString(writeHexBE(header.nBits, sizeof(header.nBits)));
      // cleanup
      params.addBoolean(resetPreviousWork);
    }
  }

  notifyMessage.write('\n');
}

bool Stratum::Prepare::prepare(Proto::BlockHeader &header, uint32_t jobVersion, BTC::CoinbaseTx &legacy, BTC::CoinbaseTx &witness, const std::vector<uint256> &merklePath, const WorkerConfig &workerCfg, const MiningConfig &miningCfg, const StratumMessage &msg)
{

}

}

