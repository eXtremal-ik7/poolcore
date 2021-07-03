#include "blockmaker/zec.h"
#include "poolcommon/arith_uint256.h"

#if ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3))
#define WANT_BUILTIN_BSWAP
#else
#define bswap_32(x) ((((x) << 24) & 0xff000000u) | (((x) << 8) & 0x00ff0000u) \
                   | (((x) >> 8) & 0x0000ff00u) | (((x) >> 24) & 0x000000ffu))
#define bswap_64(x) (((uint64_t) bswap_32((uint32_t)((x) & 0xffffffffu)) << 32) \
                   | (uint64_t) bswap_32((uint32_t)((x) >> 32)))
#endif

static inline uint32_t swab32(uint32_t v)
{
#ifdef WANT_BUILTIN_BSWAP
  return __builtin_bswap32(v);
#else
  return bswap_32(v);
#endif
}

static void diff_to_target_equi(uint32_t *target, double diff)
{
  uint64_t m;
  int k;

  for (k = 6; k > 0 && diff > 1.0; k--)
    diff /= 4294967296.0;
  m = (uint64_t)(4294901760.0 / diff);
  if (m == 0 && k == 6)
    memset(target, 0xff, 32);
  else {
    memset(target, 0, 32);
    target[k + 1] = (uint32_t)(m >> 8);
    target[k + 2] = (uint32_t)(m >> 40);
    for (k = 0; k < 28 && ((uint8_t*)target)[k] == 0; k++)
      ((uint8_t*)target)[k] = 0xff;
  }
}

double target_to_diff_equi(uint32_t* target)
{
  uint8_t* tgt = (uint8_t*) target;
  uint64_t m =
    (uint64_t)tgt[30] << 24 |
    (uint64_t)tgt[29] << 16 |
    (uint64_t)tgt[28] << 8  |
    (uint64_t)tgt[27] << 0;

  if (!m)
    return 0.;
  else
    return (double)0xffff0000UL/m;
}

static inline double getDifficulty(uint256 *target)
{
  return target_to_diff_equi(reinterpret_cast<uint32_t*>(target->begin()));
}

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

EStratumDecodeStatusTy Stratum::StratumMessage::decodeStratumMessage(const char *in, size_t size)
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
        params[2].IsString() && params[2].GetStringLength() == 8 &&
        params[3].IsString() &&
        params[4].IsString()) {
      Method = ESubmit;
      Submit.WorkerName = params[0].GetString();
      Submit.JobId = params[1].GetString();
      Submit.Time = readHexBE<uint32_t>(params[2].GetString(), 4);
      Submit.Nonce = params[3].GetString();
      Submit.Solution = params[4].GetString();
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

bool Proto::checkConsensus(const ZEC::Proto::BlockHeader &header, CheckConsensusCtx&, ZEC::Proto::ChainParams&, double *shareDiff)
{
  bool fNegative;
  bool fOverflow;
  arith_uint256 bnTarget;
  bnTarget.SetCompact(header.nBits, &fNegative, &fOverflow);

  Proto::BlockHashTy hash256 = header.GetHash();

  arith_uint256 hash = UintToArith256(hash256);
  *shareDiff = getDifficulty(&hash256);

  // Check range
  if (fNegative || bnTarget == 0 || fOverflow)
    return false;

  // Check proof of work matches claimed amount
  if (hash > bnTarget)
    return false;

  return true;
}

bool Stratum::HeaderBuilder::build(Proto::BlockHeader &header, uint32_t *jobVersion, BTC::CoinbaseTx &legacy, const std::vector<uint256> &merklePath, rapidjson::Value &blockTemplate)
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
  header.hashMerkleRoot = calculateMerkleRoot(legacy.Data.data(), legacy.Data.sizeOf(), merklePath);
  header.nTime = curtime.GetUint();
  header.nBits = strtoul(bits.GetString(), nullptr, 16);
  header.nNonce.SetNull();
  header.nSolution.resize(0);
  *jobVersion = header.nVersion;
  return true;
}

bool Stratum::CoinbaseBuilder::prepare(int64_t *blockReward, rapidjson::Value &blockTemplate)
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

  stream.seekSet(0);
  BTC::unserialize(stream, CoinbaseTx);
  if (stream.eof())
    return false;

  if (CoinbaseTx.txIn.empty() || CoinbaseTx.txOut.empty())
    return false;

  *blockReward = CoinbaseTx.txOut[0].value;
  return true;
}

void Stratum::CoinbaseBuilder::build(int64_t height,
                                     int64_t blockReward,
                                     void *coinbaseData,
                                     size_t coinbaseSize,
                                     const std::string &coinbaseMessage,
                                     const Proto::AddressTy &miningAddress,
                                     const MiningConfig&,
                                     bool,
                                     const xmstream&,
                                     BTC::CoinbaseTx &legacy,
                                     BTC::CoinbaseTx &witness)
{
  // TxIn
  {
    typename Proto::TxIn &txIn = CoinbaseTx.txIn[0];

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
    // Replace mining address and block reward
    typename Proto::TxOut &txOut = CoinbaseTx.txOut[0];
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

  BTC::Io<typename Proto::Transaction>::serialize(legacy.Data, CoinbaseTx);
  BTC::Io<typename Proto::Transaction>::serialize(witness.Data, CoinbaseTx);
}

void Stratum::Notify::build(CWork *source, typename Proto::BlockHeader &header, uint32_t, BTC::CoinbaseTx&, const std::vector<uint256>&, const MiningConfig&, bool resetPreviousWork, xmstream &notifyMessage)
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
        params.addString(writeHexLE(header.nVersion, sizeof(header.nVersion)));
      }

      // Previous block
      {
        std::string hash;
        const uint32_t *data = reinterpret_cast<const uint32_t*>(header.hashPrevBlock.begin());
        for (unsigned i = 0; i < 8; i++) {
          hash.append(writeHexLE(data[i], 4));
        }
        params.addString(hash);
      }


      // Merkle root
      {
        std::string hash;
        const uint32_t *data = reinterpret_cast<const uint32_t*>(header.hashMerkleRoot.begin());
        for (unsigned i = 0; i < 8; i++) {
          hash.append(writeHexLE(data[i], 4));
        }
        params.addString(hash);
      }

      // light client root
      {
        std::string hash;
        const uint32_t *data = reinterpret_cast<const uint32_t*>(header.hashLightClientRoot.begin());
        for (unsigned i = 0; i < 8; i++) {
          hash.append(writeHexLE(data[i], 4));
        }
        params.addString(hash);
      }
      // nTime
      params.addString(writeHexLE(header.nTime, sizeof(header.nTime)));
      // nBits
      params.addString(writeHexLE(header.nBits, sizeof(header.nBits)));
      // cleanup
      params.addBoolean(resetPreviousWork);
    }
  }

  notifyMessage.write('\n');
}

bool Stratum::Prepare::prepare(Proto::BlockHeader &header, uint32_t, BTC::CoinbaseTx&, BTC::CoinbaseTx&, const std::vector<uint256>&, const WorkerConfig &workerCfg, const MiningConfig &miningCfg, const StratumMessage &msg)
{
  header.nTime = swab32(msg.Submit.Time);

  if (msg.Submit.Nonce.size() == 64) {
    hex2bin(msg.Submit.Nonce.data(), 64, header.nNonce.begin());
  } else if (msg.Submit.Nonce.size() == (32-miningCfg.FixedExtraNonceSize)*2) {
    writeBinBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize, header.nNonce.begin());
    hex2bin(msg.Submit.Nonce.data(), msg.Submit.Nonce.size(), header.nNonce.begin() + miningCfg.FixedExtraNonceSize);
  } else {
    return false;
  }

  header.nSolution.resize(1344);
  if (msg.Submit.Solution.size() == 1344*2) {
    hex2bin(msg.Submit.Solution.data(), msg.Submit.Solution.size(), header.nSolution.data());
  } else if (msg.Submit.Solution.size() == 1347*2) {
    hex2bin(msg.Submit.Solution.data() + 6, msg.Submit.Solution.size() - 6, header.nSolution.data());
  } else {
    return false;
  }

  return true;
}

void Stratum::buildSendTargetMessage(xmstream &stream, double difficulty)
{
  uint8_t target[32];
  diff_to_target_equi(reinterpret_cast<uint32_t*>(target), difficulty);
  std::reverse(target, target+sizeof(target));
  {
    JSON::Object object(stream);
    object.addString("method", "mining.set_target");
    object.addField("params");
    {
      JSON::Array params(stream);
      params.addHex(target, 32);
    }
  }
}

}

