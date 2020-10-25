// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockmaker/xpm.h"
#include "blockmaker/serializeJson.h"
#include "poolcommon/arith_uint256.h"
#include "poolcommon/utils.h"
#include "poolcore/backendData.h"
#include "blockmaker/merkleTree.h"

namespace BTC {

void Io<mpz_class>::serialize(xmstream &dst, const mpz_class &data)
{
  constexpr mp_limb_t one = 1;
  auto bnSize = mpz_sizeinbase(data.get_mpz_t(), 256);
  auto serializedSize = bnSize;
  if (data.get_mpz_t()->_mp_size) {
      mp_limb_t signBit = one << (8*(bnSize % sizeof(mp_limb_t)) - 1);
      if (data.get_mpz_t()->_mp_d[data.get_mpz_t()->_mp_size-1] & signBit)
          serializedSize++;
  }

  serializeVarSize(dst, serializedSize);
  mpz_export(dst.reserve<uint8_t>(bnSize), nullptr, -1, 1, -1, 0, data.get_mpz_t());
  if (serializedSize > bnSize)
    dst.write<uint8_t>(0);
}

void Io<mpz_class>::unserialize(xmstream &src, mpz_class &data)
{
  uint64_t size;
  unserializeVarSize(src, size);
  if (const uint8_t *p = src.seek<uint8_t>(size))
    mpz_import(data.get_mpz_t(), size, -1, 1, -1, 0, p);
}

void Io<mpz_class>::unpack(xmstream &src, DynamicPtr<mpz_class> dst)
{
  uint64_t size;
  unserializeVarSize(src, size);
  if (const uint8_t *p = src.seek<uint8_t>(size)) {
    size_t alignedSize = size % sizeof(mp_limb_t) ? size + (sizeof(mp_limb_t) - size % sizeof(mp_limb_t)) : size;
    size_t limbsNum = alignedSize / sizeof(mp_limb_t);
    size_t dataOffset = dst.stream().offsetOf();

    dst.stream().reserve<mp_limb_t>(limbsNum);

    mpz_class *mpz = dst.ptr();
    mp_limb_t *data = reinterpret_cast<mp_limb_t*>(dst.stream().data<uint8_t>() + dataOffset);
    mpz->get_mpz_t()->_mp_d = data;
    mpz->get_mpz_t()->_mp_size = static_cast<int>(limbsNum);
    mpz->get_mpz_t()->_mp_alloc = static_cast<int>(limbsNum);
    mpz_import(mpz->get_mpz_t(), size, -1, 1, -1, 0, p);

    // Change address to stream offset
    mpz->get_mpz_t()->_mp_d = reinterpret_cast<mp_limb_t*>(dataOffset);
  }
}

void Io<mpz_class>::unpackFinalize(DynamicPtr<mpz_class> dst)
{
  mpz_class *mpz = dst.ptr();
  mpz->get_mpz_t()->_mp_d = reinterpret_cast<mp_limb_t*>(dst.stream().data<uint8_t>() + reinterpret_cast<size_t>(mpz->get_mpz_t()->_mp_d));
}

void Io<XPM::Proto::BlockHeader>::serialize(xmstream &dst, const XPM::Proto::BlockHeader &data)
{
  BTC::serialize(dst, data.nVersion);
  BTC::serialize(dst, data.hashPrevBlock);
  BTC::serialize(dst, data.hashMerkleRoot);
  BTC::serialize(dst, data.nTime);
  BTC::serialize(dst, data.nBits);
  BTC::serialize(dst, data.nNonce);
  BTC::serialize(dst, data.bnPrimeChainMultiplier);
}

void Io<XPM::Proto::BlockHeader>::unserialize(xmstream &src, XPM::Proto::BlockHeader &data)
{
  BTC::unserialize(src, data.nVersion);
  BTC::unserialize(src, data.hashPrevBlock);
  BTC::unserialize(src, data.hashMerkleRoot);
  BTC::unserialize(src, data.nTime);
  BTC::unserialize(src, data.nBits);
  BTC::unserialize(src, data.nNonce);
  BTC::unserialize(src, data.bnPrimeChainMultiplier);
}

void Io<XPM::Proto::BlockHeader>::unpack(xmstream &src, DynamicPtr<XPM::Proto::BlockHeader> dst)
{
  XPM::Proto::BlockHeader *ptr = dst.ptr();

  // TODO: memcpy on little-endian
  BTC::unserialize(src, ptr->nVersion);
  BTC::unserialize(src, ptr->hashPrevBlock);
  BTC::unserialize(src, ptr->hashMerkleRoot);
  BTC::unserialize(src, ptr->nTime);
  BTC::unserialize(src, ptr->nBits);
  BTC::unserialize(src, ptr->nNonce);

  // unserialize prime chain multiplier
  BTC::unpack(src, DynamicPtr<mpz_class>(dst.stream(), dst.offset()+offsetof(XPM::Proto::BlockHeader, bnPrimeChainMultiplier)));
}

void Io<XPM::Proto::BlockHeader>::unpackFinalize(DynamicPtr<XPM::Proto::BlockHeader> dst)
{
  BTC::unpackFinalize(DynamicPtr<mpz_class>(dst.stream(), dst.offset()+offsetof(XPM::Proto::BlockHeader, bnPrimeChainMultiplier)));
}
}

namespace XPM {
// XPM Proof of work
static constexpr int nFractionalBits = 24;
static constexpr unsigned TARGET_FRACTIONAL_MASK = (1u<<nFractionalBits) - 1;
static constexpr unsigned TARGET_LENGTH_MASK = ~TARGET_FRACTIONAL_MASK;
static constexpr uint64_t nFractionalDifficultyMax = (1llu << (nFractionalBits + 32));
static constexpr uint64_t nFractionalDifficultyMin = (1llu << 32);
static constexpr uint64_t nFractionalDifficultyThreshold = (1llu << (8 + 32));
static constexpr unsigned nWorkTransitionRatio = 32;

static inline unsigned int TargetGetLength(unsigned int nBits)
{
  return ((nBits & TARGET_LENGTH_MASK) >> nFractionalBits);
}

static inline unsigned int TargetGetFractional(unsigned int nBits)
{
    return (nBits & TARGET_FRACTIONAL_MASK);
}

/// Returns value of hyperbolic function of difficulty fractional part
/// range:
///   f(0.000000000000000000000000) = 2^32 = 4294967296
///   f(0.999999940395355224609375) = 2^56 = 72057594037927936
uint64_t TargetGetFractionalDifficulty(unsigned int nBits)
{
  return nFractionalDifficultyMax / static_cast<uint64_t>((1llu << nFractionalBits) - TargetGetFractional(nBits));
}

static inline unsigned int TargetFromInt(unsigned int nLength)
{
    return (nLength << nFractionalBits);
}

static uint32_t fractionalPart( XPM::Proto::CheckConsensusCtx &ctx)
{
  // Calculate fractional length
  // ctx.exp = ((ctx.bn - ctx.result) << nFractionalBits) / ctx.bn
  mpz_sub(ctx.exp, ctx.bn, ctx.FermatResult);
  mpz_mul_2exp(ctx.exp, ctx.exp, nFractionalBits);
  mpz_div(ctx.exp, ctx.exp, ctx.bn);
  return static_cast<uint32_t>(mpz_get_ui(ctx.exp)) & ((1 << nFractionalBits) - 1);
}

static inline double getPrimeDifficulty(unsigned int nBits)
{
    return ((double) nBits / (double) (1 << nFractionalBits));
}

static bool FermatProbablePrimalityTest(XPM::Proto::CheckConsensusCtx &ctx)
{
  // FermatResult = 2^(bn - 1) mod bn
  mpz_sub_ui(ctx.exp, ctx.bn, 1);
  mpz_powm(ctx.FermatResult, ctx.two, ctx.exp, ctx.bn);
  return mpz_cmp_ui(ctx.FermatResult, 1) == 0;
}

static bool EulerLagrangeLifchitzPrimalityTest(XPM::Proto::CheckConsensusCtx &ctx, bool isSophieGermain)
{
  // EulerResult = 2^((bn - 1)/2) mod bn
  mpz_sub_ui(ctx.exp, ctx.bn, 1);
  mpz_fdiv_q_2exp(ctx.exp, ctx.exp, 1);
  mpz_powm(ctx.EulerResult, ctx.two, ctx.exp, ctx.bn);

  // FermatResult = (EulerResult ^ 2) mod bn = 2^(bn - 1) mod bn
  mpz_mul(ctx.FermatResult, ctx.EulerResult, ctx.EulerResult);
  mpz_mod(ctx.FermatResult, ctx.FermatResult, ctx.bn);

  auto mod8 = mpz_get_ui(ctx.bn) % 8;
  bool passedTest = false;
  if (isSophieGermain && (mod8 == 7)) {
    passedTest = mpz_cmp_ui(ctx.EulerResult, 1) == 0;
  } else if (isSophieGermain && (mod8 == 3)) {
    mpz_add_ui(ctx.exp, ctx.EulerResult, 1);
    passedTest = mpz_cmp(ctx.exp, ctx.bn) == 0;
  } else if ((!isSophieGermain) && (mod8 == 5)) {
    mpz_add_ui(ctx.exp, ctx.EulerResult, 1);
    passedTest = mpz_cmp(ctx.exp, ctx.bn) == 0;
  } else if ((!isSophieGermain) && (mod8 == 1)) {
    passedTest = mpz_cmp_ui(ctx.EulerResult, 1) == 0;
  }

  return passedTest;
}

static unsigned primeChainLength(XPM::Proto::CheckConsensusCtx &ctx, bool isSophieGermain)
{
  if (!FermatProbablePrimalityTest(ctx))
    return fractionalPart(ctx);

  uint32_t rationalPart = 0;
  while (true) {
    rationalPart++;

    // Calculate next number in Cunningham chain
    // 1CC (Sophie Germain) bn = bn*2 + 1
    // 2CC bn = bn*2 - 1
    mpz_mul_2exp(ctx.bn, ctx.bn, 1);
    if (isSophieGermain)
      mpz_add_ui(ctx.bn, ctx.bn, 1);
    else
      mpz_sub_ui(ctx.bn, ctx.bn, 1);

    bool EulerTestPassed = EulerLagrangeLifchitzPrimalityTest(ctx, isSophieGermain);
    bool FermatTestPassed = mpz_cmp_ui(ctx.FermatResult, 1) == 0;
    if (EulerTestPassed != FermatTestPassed) {
      // Euler-Lagrange-Lifchitz and Fermat tests gives different results!
      return 0;
    }

    if (!EulerTestPassed)
      return (rationalPart << nFractionalBits) | fractionalPart(ctx);
  }
}

/// Get length of chain type 1 / Sophie Germain (n, 2n+1, ...)
static inline uint32_t c1Length(XPM::Proto::CheckConsensusCtx &ctx)
{
  mpz_sub_ui(ctx.bn, ctx.bnPrimeChainOrigin, 1);
  return primeChainLength(ctx, true);
}

/// Get length of chain type 2 (n, 2n-1, ...)
static inline uint32_t c2Length(XPM::Proto::CheckConsensusCtx &ctx)
{
  mpz_add_ui(ctx.bn, ctx.bnPrimeChainOrigin, 1);
  return primeChainLength(ctx, false);
}

/// Get length of bitwin chain
static inline uint32_t bitwinLength(uint32_t l1, uint32_t l2)
{
  return TargetGetLength(l1) > TargetGetLength(l2) ?
    (l2 + TargetFromInt(TargetGetLength(l2)+1)) :
    (l1 + TargetFromInt(TargetGetLength(l1)));
}

void Proto::checkConsensusInitialize(Proto::CheckConsensusCtx &ctx)
{
  mpz_init(ctx.bnPrimeChainOrigin);
  mpz_init(ctx.bn);
  mpz_init(ctx.exp);
  mpz_init(ctx.EulerResult);
  mpz_init(ctx.FermatResult);
  mpz_init(ctx.two);
  mpz_set_ui(ctx.two, 2);
}

bool Proto::checkConsensus(const Proto::BlockHeader &header, Proto::CheckConsensusCtx &ctx, Proto::ChainParams &chainParams, uint64_t *shareSize)
{
  // Check target
  *shareSize = 0;
  if (TargetGetLength(header.nBits) < chainParams.minimalChainLength || TargetGetLength(header.nBits) > 99)
    return false;

  XPM::Proto::BlockHashTy hash = header.GetOriginalHeaderHash();

  {
    // Check header hash limit (most significant bit of hash must be 1)
    uint8_t hiByte = *(hash.begin() + 31);
    if (!(hiByte & 0x80))
      return false;
  }

  // Check target for prime proof-of-work
  uint256ToBN(ctx.bnPrimeChainOrigin, hash);
  mpz_mul(ctx.bnPrimeChainOrigin, ctx.bnPrimeChainOrigin, header.bnPrimeChainMultiplier.get_mpz_t());

  auto bnPrimeChainOriginBitSize = mpz_sizeinbase(ctx.bnPrimeChainOrigin, 2);
  if (bnPrimeChainOriginBitSize < 255)
    return false;
  if (bnPrimeChainOriginBitSize > 2000)
    return false;

  uint32_t l1 = c1Length(ctx);
  uint32_t l2 = c2Length(ctx);
  uint32_t lbitwin = bitwinLength(l1, l2);
  if (!(l1 >= header.nBits || l2 >= header.nBits || lbitwin >= header.nBits))
    return false;

  uint32_t chainLength;
  chainLength = std::max(l1, l2);
  chainLength = std::max(chainLength, lbitwin);
  *shareSize = TargetGetLength(chainLength);

  // Check that the certificate (bnPrimeChainMultiplier) is normalized
  if (mpz_get_ui(header.bnPrimeChainMultiplier.get_mpz_t()) % 2 == 0 && mpz_get_ui(ctx.bnPrimeChainOrigin) % 4 == 0) {
     mpz_fdiv_q_2exp(ctx.bnPrimeChainOrigin, ctx.bnPrimeChainOrigin, 1);

     // Calculate extended C1, C2 & bitwin chain lengths
     mpz_sub_ui(ctx.bn, ctx.bnPrimeChainOrigin, 1);
     uint32_t l1Extended = FermatProbablePrimalityTest(ctx) ? l1 + (1 << nFractionalBits) : fractionalPart(ctx);
     mpz_add_ui(ctx.bn, ctx.bnPrimeChainOrigin, 1);
     uint32_t l2Extended = FermatProbablePrimalityTest(ctx) ? l2 + (1 << nFractionalBits) : fractionalPart(ctx);
     uint32_t lbitwinExtended = bitwinLength(l1Extended, l2Extended);
     if (l1Extended > chainLength || l2Extended > chainLength || lbitwinExtended > chainLength)
       return false;
  }

  // TODO: store chain length and type (for what?)
  return true;
}

void Zmq::initializeMiningConfig(MiningConfig &cfg, rapidjson::Value &instanceCfg)
{
  if (instanceCfg.HasMember("minShareLength") && instanceCfg["minShareLength"].IsUint())
    cfg.MinShareLength = instanceCfg["minShareLength"].GetUint();
}

void Zmq::initializeThreadConfig(ThreadConfig &cfg, unsigned threadId, unsigned threadsNum)
{
  cfg.ExtraNonceCurrent = threadId;
  cfg.ThreadsNum = threadsNum;
}

void Zmq::resetThreadConfig(ThreadConfig &cfg)
{
  cfg.ExtraNonceMap.clear();
}

void Zmq::buildBlockProto(Work &work, MiningConfig &miningCfg, pool::proto::Block &proto)
{
  proto.set_height(static_cast<uint32_t>(work.Height));
  proto.set_hash(work.Header.hashPrevBlock.ToString());
  proto.set_prevhash(work.Header.hashPrevBlock.ToString());
  proto.set_reqdiff(0);
  proto.set_minshare(miningCfg.MinShareLength);
}

void Zmq::buildWorkProto(Work &work, pool::proto::Work &proto)
{
  // TODO: switch to 64-bit height
  proto.set_height(static_cast<uint32_t>(work.Height));
  proto.set_merkle(work.Header.hashMerkleRoot.ToString().c_str());
  proto.set_time(std::max(static_cast<uint32_t>(time(0)), work.Header.nTime));
  proto.set_bits(work.Header.nBits);
}

void Zmq::generateNewWork(Work &work, WorkerConfig &workerCfg, ThreadConfig &threadCfg, MiningConfig &miningCfg)
{
  Proto::BlockHeader &header = work.Header;

  // Update extra nonce
  workerCfg.ExtraNonceFixed = threadCfg.ExtraNonceCurrent;

  // Write target extra nonce to first txin
  uint8_t *scriptSig = work.FirstTxData.data<uint8_t>() + work.TxExtraNonceOffset;
  writeBinBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize, scriptSig);

  // Recalculate merkle root
  header.hashMerkleRoot = calculateMerkleRoot(work.FirstTxData.data(), work.FirstTxData.sizeOf(), work.MerklePath);

  // Update thread config
  threadCfg.ExtraNonceMap[work.Header.hashMerkleRoot] = workerCfg.ExtraNonceFixed;
  threadCfg.ExtraNonceCurrent += threadCfg.ThreadsNum;
}

bool Zmq::prepareToSubmit(Work &work, ThreadConfig &threadCfg, MiningConfig &miningCfg, pool::proto::Request &req, pool::proto::Reply &rep)
{
  const pool::proto::Share& share = req.share();

  Proto::BlockHeader &header = work.Header;
  header.nTime = share.time();
  header.nNonce = share.nonce();
  header.hashMerkleRoot = uint256S(share.merkle());
  header.bnPrimeChainMultiplier.set_str(share.multi().c_str(), 16);

  // merkle must be present at extraNonce map
  auto nonceIt = threadCfg.ExtraNonceMap.find(header.hashMerkleRoot);
  if (nonceIt == threadCfg.ExtraNonceMap.end()) {
    rep.set_error(pool::proto::Reply::STALE);
    return false;
  }

  // Write extra nonce
  uint64_t extraNonceFixed = nonceIt->second;
  uint8_t *scriptSig = work.FirstTxData.data<uint8_t>() + work.TxExtraNonceOffset;
  writeBinBE(extraNonceFixed, miningCfg.FixedExtraNonceSize, scriptSig);
  bin2hexLowerCase(scriptSig, work.BlockHexData.data<char>() + work.BlockHexExtraNonceOffset, miningCfg.FixedExtraNonceSize);

  {
    // Update header in block hex dump
    char buffer[4096];
    xmstream stream(buffer, sizeof(buffer));
    stream.reset();
    BTC::X::serialize(stream, header);
    bin2hexLowerCase(buffer, work.BlockHexData.data<char>(), stream.sizeOf());
  }

  return true;
}

bool Zmq::loadFromTemplate(Work &work, rapidjson::Value &document, const MiningConfig &cfg, PoolBackend *backend, const std::string &, Proto::AddressTy &miningAddress, const std::string &coinbaseMsg, std::string &error)
{
  char buffer[4096];
  xmstream stream(buffer, sizeof(buffer));
  work.Height = 0;
  work.MerklePath.clear();
  work.FirstTxData.reset();
  work.BlockHexData.reset();
  work.Backend = backend;

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

  work.Height = height.GetUint64();
  Proto::BlockHeader &header = work.Header;
  header.nVersion = version.GetUint();
  header.hashPrevBlock.SetHex(hashPrevBlock.GetString());
  header.hashMerkleRoot.SetNull();
  header.nTime = curtime.GetUint();
  header.nBits = strtoul(bits.GetString(), nullptr, 16);
  header.nNonce = 0;
  header.bnPrimeChainMultiplier = 0;

  // Serialize header and transactions count
  stream.reset();
  BTC::X::serialize(stream, header);
  BTC::serializeVarSize(stream, transactions.Size() + 1);
  bin2hexLowerCase(stream.data(), work.BlockHexData.reserve<char>(stream.sizeOf()*2), stream.sizeOf());

  // Coinbase
  Proto::Transaction coinbaseTx;
  coinbaseTx.version = 1;

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
    work.ScriptSigExtraNonceOffset = static_cast<unsigned>(scriptsig.offsetOf());
    work.TxExtraNonceOffset = static_cast<unsigned>(work.ScriptSigExtraNonceOffset + coinbaseTx.getFirstScriptSigOffset(false));
    for (size_t i = 0, ie = cfg.FixedExtraNonceSize; i != ie; ++i)
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
  }

  coinbaseTx.lockTime = 0;
  work.BlockHexExtraNonceOffset = static_cast<unsigned>(work.BlockHexData.sizeOf() + work.TxExtraNonceOffset*2);
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

  // Build merkle path
  dumpMerkleTree(txHashes, work.MerklePath);
  work.TransactionsNum = transactions.Size() + 1;
  return true;
}
}

void serializeJsonInside(xmstream &stream, const XPM::Proto::BlockHeader &header)
{
  std::string bnPrimeChainMultiplier = header.bnPrimeChainMultiplier.get_str();
  serializeJson(stream, "version", header.nVersion); stream.write(',');
  serializeJson(stream, "hashPrevBlock", header.hashPrevBlock); stream.write(',');
  serializeJson(stream, "hashMerkleRoot", header.hashMerkleRoot); stream.write(',');
  serializeJson(stream, "time", header.nTime); stream.write(',');
  serializeJson(stream, "bits", header.nBits); stream.write(',');
  serializeJson(stream, "nonce", header.nNonce); stream.write(',');
  serializeJson(stream, "bnPrimeChainMultiplier", bnPrimeChainMultiplier);
}
