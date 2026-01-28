// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockmaker/xpm.h"
#include "blockmaker/serializeJson.h"
#include "poolcommon/utils.h"
#include "blockmaker/merkleTree.h"

CDivisionChecker XPM::Zmq::DivisionChecker_;

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

static int64_t getFee(size_t nBytes_, bool isProtocolV1)
{
  static constexpr int64_t COIN = 100000000;
  static constexpr int64_t nSatoshisPerK = COIN;
  int64_t nSize = int64_t(nBytes_);
  int64_t nFee = nSatoshisPerK * nSize / 1000;

  if(isProtocolV1) {
    nFee = (1 + (nSize / 1000)) * nSatoshisPerK;
  }

  if (nFee == 0 && nSize != 0) {
    if (nSatoshisPerK > 0)
      nFee = int64_t(1);
    if (nSatoshisPerK < 0)
      nFee = int64_t(-1);
  }

  return nFee;
}

static int64_t targetGetMint(unsigned int nBits)
{
  static constexpr uint32_t COIN = 100000000;
  static constexpr uint32_t CENT = 1000000;

  uint64_t nMint = 0;
  mpz_class bnMint = 999;
  bnMint *= COIN;
  bnMint = (bnMint << nFractionalBits) / nBits;
  bnMint = (bnMint << nFractionalBits) / nBits;
  bnMint = (bnMint / CENT) * CENT;  // mint value rounded to cent
  {
    size_t limbsNumber = 0;
    mpz_export(&nMint, &limbsNumber, -1, 8, 0, 0, bnMint.get_mpz_t());
  }
  return nMint;
}

static unsigned getSmallestDivisor(const CDivisionChecker &checker32,
                                   mpz_t origin,
                                   Proto::EChainType type,
                                   unsigned,
                                   unsigned target,
                                   unsigned rangeBegin,
                                   unsigned rangeEnd)
{
  mpz_class localOrigin(origin);

  unsigned divisor = -1U;
  size_t limbsNumber;
  uint32_t limbs[2048 / 32];
  mpz_export(limbs, &limbsNumber, -1, 4, 0, 0, origin);

  unsigned lastPrimeIndex = rangeEnd == -1U ? 131072 : rangeEnd;
  if (type == Proto::ECunninghamChain1) {
    for (unsigned primeIdx = rangeBegin; primeIdx < lastPrimeIndex; primeIdx++) {
      int prime = checker32.prime(primeIdx);
      int remainder = checker32.mod32(limbs, limbsNumber, primeIdx);

      remainder--;
      if (remainder < 0)
        remainder = prime - 1;

      bool isDivisor = false;
      for (unsigned i = 0; i < target; i++) {
        if (remainder == 0) {
          isDivisor = true;
          break;
        }
        remainder = remainder * 2 + 1;
        if (remainder >= prime)
          remainder -= prime;
      }

      if (isDivisor) {
        divisor = primeIdx;
        break;
      }
    }
  } else if (type == Proto::ECunninghamChain2) {
    for (unsigned primeIdx = rangeBegin; primeIdx < lastPrimeIndex; primeIdx++) {
      int prime = checker32.prime(primeIdx);
      int remainder = checker32.mod32(limbs, limbsNumber, primeIdx);

      remainder++;
      if (remainder >= prime)
        remainder -= prime;

      bool isDivisor = false;
      for (unsigned i = 0; i < target; i++) {
        if (remainder == 0) {
          isDivisor = true;
          break;
        }
        remainder = remainder * 2 - 1;
        if (remainder >= prime)
          remainder -= prime;
      }

      if (isDivisor) {
        divisor = primeIdx;
        break;
      }
    }
  } else {
    for (unsigned primeIdx = rangeBegin; primeIdx < lastPrimeIndex; primeIdx++) {
      unsigned c1Target = target/2 + (target % 2);
      unsigned c2Target = target/2;
      int prime = checker32.prime(primeIdx);
      int remOrigin = checker32.mod32(limbs, limbsNumber, primeIdx);
      bool isDivisor = false;
      {
        int remainder = remOrigin - 1;
        if (remainder < 0)
          remainder = prime - 1;
        for (unsigned i = 0; i < c1Target; i++) {
          if (remainder == 0) {
            isDivisor = true;
            break;
          }
          remainder = remainder * 2 + 1;
          if (remainder >= prime)
            remainder -= prime;
        }

        if (isDivisor) {
          divisor = primeIdx;
          break;
        }
      }
      {
        int remainder = remOrigin + 1;
        if (remainder >= prime)
          remainder -= prime;
        for (unsigned i = 0; i < c2Target; i++) {
          if (remainder == 0) {
            isDivisor = true;
            break;
          }
          remainder = remainder * 2 - 1;
          if (remainder >= prime)
            remainder -= prime;
        }

        if (isDivisor) {
          divisor = primeIdx;
          break;
        }
      }
    }
  }

  return divisor;
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

bool Proto::checkConsensus(const Proto::BlockHeader &header,
                           Proto::CheckConsensusCtx &ctx,
                           Proto::ChainParams &chainParams,
                           double *shareSize,
                           EChainType *chainType)
{
  // Check target
  *shareSize = 0;
  if (TargetGetLength(header.nBits) < chainParams.minimalChainLength || TargetGetLength(header.nBits) > 99)
    return false;

  UInt<256> hash = header.GetOriginalHeaderHash();

  // Check header hash limit (most significant bit of hash must be 1)
  if (!(hash.data()[3] & 0x8000000000000000ULL))
    return false;

  // Check target for prime proof-of-work
  uintToMpz(ctx.bnPrimeChainOrigin, hash);
  mpz_mul(ctx.bnPrimeChainOrigin, ctx.bnPrimeChainOrigin, header.bnPrimeChainMultiplier.get_mpz_t());

  auto bnPrimeChainOriginBitSize = mpz_sizeinbase(ctx.bnPrimeChainOrigin, 2);
  if (bnPrimeChainOriginBitSize < 255)
    return false;
  if (bnPrimeChainOriginBitSize > 2000)
    return false;

  uint32_t chainLength;
  uint32_t l1 = c1Length(ctx);
  uint32_t l2 = c2Length(ctx);
  uint32_t lbitwin = bitwinLength(l1, l2);
  chainLength = std::max(l1, l2);
  chainLength = std::max(chainLength, lbitwin);

  *shareSize = getPrimeDifficulty(chainLength);
  if (chainLength == l1)
    *chainType = ECunninghamChain1;
  else if (chainLength == l2)
    *chainType = ECunninghamChain2;
  else
    *chainType = EBiTwin;

  if (!(l1 >= header.nBits || l2 >= header.nBits || lbitwin >= header.nBits))
    return false;

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
  proto.set_hash(work.Header.hashPrevBlock.getHexLE());
  proto.set_prevhash(work.Header.hashPrevBlock.getHexLE());
  proto.set_reqdiff(0);
  proto.set_minshare(miningCfg.MinShareLength);
}

void Zmq::buildWorkProto(Work &work, pool::proto::Work &proto)
{
  proto.set_version(work.Header.nVersion);
  proto.set_height(static_cast<uint32_t>(work.Height));
  proto.set_merkle(work.Header.hashMerkleRoot.getHexLE().c_str());
  proto.set_time(std::max(static_cast<uint32_t>(time(0)), work.Header.nTime));
  proto.set_bits(work.Header.nBits);
}

void Zmq::generateNewWork(Work &work, WorkerConfig &workerCfg, ThreadConfig &threadCfg, MiningConfig &miningCfg)
{
  Proto::BlockHeader &header = work.Header;

  // Write target extra nonce to first txin
  uint8_t *scriptSig = work.CoinbaseTx.data<uint8_t>() + work.TxExtraNonceOffset;
  writeBinBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize, scriptSig);

  // Recalculate merkle root
  {
    BaseBlob<256> coinbaseTxHash;
    CCtxSha256 sha256;
    sha256Init(&sha256);
    sha256Update(&sha256, work.CoinbaseTx.data(), work.CoinbaseTx.sizeOf());
    sha256Final(&sha256, coinbaseTxHash.begin());
    sha256Init(&sha256);
    sha256Update(&sha256, coinbaseTxHash.begin(), coinbaseTxHash.size());
    sha256Final(&sha256, coinbaseTxHash.begin());
    header.hashMerkleRoot = calculateMerkleRootWithPath(coinbaseTxHash, &work.MerklePath[0], work.MerklePath.size(), 0);
  }

  // Update thread config
  threadCfg.ExtraNonceMap[work.Header.hashMerkleRoot] = workerCfg.ExtraNonceFixed;
}

bool Zmq::prepareToSubmit(Work &work, ThreadConfig &threadCfg, MiningConfig &miningCfg, pool::proto::Request &req, pool::proto::Reply &rep)
{
  const pool::proto::Share& share = req.share();

  Proto::BlockHeader &header = work.Header;
  header.nTime = share.time();
  header.nNonce = share.nonce();
  header.hashMerkleRoot = BaseBlob<256>::fromHexLE(share.merkle().c_str());
  header.bnPrimeChainMultiplier.set_str(share.multi().c_str(), 16);

  // merkle must be present at extraNonce map
  auto nonceIt = threadCfg.ExtraNonceMap.find(header.hashMerkleRoot);
  if (nonceIt == threadCfg.ExtraNonceMap.end()) {
    rep.set_error(pool::proto::Reply::STALE);
    return false;
  }

  // Write extra nonce
  std::string extraNonceHex = writeHexBE(nonceIt->second, miningCfg.FixedExtraNonceSize);
  memcpy(work.TxHexData.data<uint8_t>() + work.BlockHexExtraNonceOffset, &extraNonceHex[0], extraNonceHex.size());

  {
    char buffer[4096];
    xmstream stream(buffer, sizeof(buffer));
    stream.reset();
    BTC::X::serialize(stream, header);

    work.BlockHexData.reset();
    bin2hexLowerCase(stream.data(), work.BlockHexData.reserve<char>(stream.sizeOf()*2), stream.sizeOf());
    work.BlockHexData.write(work.TxHexData.data(), work.TxHexData.sizeOf());
  }

  return true;
}

bool Zmq::loadFromTemplate(Work &work, rapidjson::Value &document, const MiningConfig &cfg, PoolBackend *backend, const std::string &ticker, Proto::AddressTy &miningAddress, const std::string &coinbaseMsg, std::string &error)
{
  work.Height = 0;
  work.MerklePath.clear();
  work.CoinbaseTx.reset();
  work.TxHexData.reset();
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
  rapidjson::Value::Array transactions = blockTemplate["transactions"].GetArray();
  if (!height.IsUint64() ||
      !version.IsUint() ||
      !hashPrevBlock.IsString() ||
      !curtime.IsUint() ||
      !bits.IsString()) {
    error = "height or header data invalid format";
    return false;
  }

  work.Height = height.GetUint64();
  Proto::BlockHeader &header = work.Header;
  header.nVersion = version.GetUint();
  header.hashPrevBlock.setHexLE(hashPrevBlock.GetString());
  header.hashMerkleRoot.setNull();
  header.nTime = curtime.GetUint();
  header.nBits = strtoul(bits.GetString(), nullptr, 16);
  header.nNonce = 0;
  header.bnPrimeChainMultiplier = 0;

  // Serialize header and transactions count
  {
    uint8_t txNumSerialized[16];
    xmstream stream(txNumSerialized, sizeof(txNumSerialized));
    stream.reset();
    BTC::serializeVarSize(stream, transactions.Size() + 1);
    bin2hexLowerCase(stream.data(), work.TxHexData.reserve<char>(stream.sizeOf()*2), stream.sizeOf());
  }

  // Coinbase
  Proto::Transaction coinbaseTx;
  coinbaseTx.version = 1;

  // TxIn
  {
    coinbaseTx.txIn.resize(1);
    BTC::Proto::TxIn &txIn = coinbaseTx.txIn[0];
    txIn.previousOutputHash.setNull();
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
    coinbaseTx.txOut.resize(1);

    {
      // First txout (funds)
      BTC::Proto::TxOut &txOut = coinbaseTx.txOut[0];
      work.BlockReward = 0;
      txOut.value = 0;

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
  work.BlockHexExtraNonceOffset = static_cast<unsigned>(work.TxHexData.sizeOf() + work.TxExtraNonceOffset*2);
  BTC::X::serialize(work.CoinbaseTx, coinbaseTx);

  if (ticker == "DTC" || ticker == "DTC.testnet" || ticker == "DTC.regtest") {
    // TODO: make separate 'blockmaker' for DTC
    work.CoinbaseTx.write<uint8_t>(0);
  }

  // Calculate reward & serialize coinbase transaction again
  work.BlockReward = coinbaseTx.txOut[0].value = targetGetMint(work.Header.nBits) - getFee(work.CoinbaseTx.sizeOf(), false);
  work.CoinbaseTx.reset();
  BTC::X::serialize(work.CoinbaseTx, coinbaseTx);

  bin2hexLowerCase(work.CoinbaseTx.data(), work.TxHexData.reserve<char>(work.CoinbaseTx.sizeOf()*2), work.CoinbaseTx.sizeOf());

  // Transactions
  std::vector<BaseBlob<256>> txHashes;
  txHashes.emplace_back();
  txHashes.back().setNull();
  for (rapidjson::SizeType i = 0, ie = transactions.Size(); i != ie; ++i) {
    rapidjson::Value &txSrc = transactions[i];
    if (!txSrc.HasMember("data") || !txSrc["data"].IsString()) {
      error = "no 'data' for transaction";
      return false;
    }

    work.TxHexData.write(txSrc["data"].GetString(), txSrc["data"].GetStringLength());
    if (!txSrc.HasMember("txid") || !txSrc["txid"].IsString()) {
      error = "no 'hash' for transaction";
      return false;
    }

    txHashes.emplace_back();
    txHashes.back().setHexLE(txSrc["txid"].GetString());
  }

  // Build merkle path
  buildMerklePath(txHashes, 0, work.MerklePath);
  work.TransactionsNum = transactions.Size() + 1;



  return true;
}

UInt<256> Zmq::Work::expectedWork()
{
  int headerChainLength = TargetGetLength(Header.nBits);
  double difficulty = getPrimeDifficulty(Header.nBits);
  double fracDiff = difficulty - floor(difficulty);
  double work = pow(10, headerChainLength - 7) / (0.97 * (1 - fracDiff) + 0.03);
  return UInt<256>::fromDouble(work * 4294967296.0);
}

UInt<256> Zmq::Work::shareWork(Proto::CheckConsensusCtx &ctx,
                            double shareDiff,
                            double shareTarget,
                            const CExtraInfo &info,
                            WorkerConfig &workerConfig)
{
  constexpr unsigned ShareWindowSize = 128;

  UInt<256> hash = Header.GetOriginalHeaderHash();
  uintToMpz(ctx.bnPrimeChainOrigin, hash);
  mpz_mul(ctx.bnPrimeChainOrigin, ctx.bnPrimeChainOrigin, Header.bnPrimeChainMultiplier.get_mpz_t());
  unsigned bnPrimeChainOriginBitSize = mpz_sizeinbase(ctx.bnPrimeChainOrigin, 2);
  unsigned chainLength = static_cast<unsigned>(shareDiff);
  int headerChainLength = std::max(TargetGetLength(Header.nBits), static_cast<unsigned>(shareTarget));

  unsigned smallestDivisor = getSmallestDivisor(Zmq::DivisionChecker_,
                                                ctx.bnPrimeChainOrigin,
                                                info.ChainType,
                                                chainLength,
                                                headerChainLength,
                                                0,
                                                workerConfig.WeaveDepth);
  if (smallestDivisor != -1U)
    return UInt<256>::zero();

  workerConfig.SharesBitSize.push_back(bnPrimeChainOriginBitSize);
  if (workerConfig.SharesBitSize.size() >= ShareWindowSize)
    workerConfig.SharesBitSize.pop_front();

  double bnPrimeChainOriginBitSizeAvg = 0.0;
  for (unsigned i = 0; i < workerConfig.SharesBitSize.size(); i++)
    bnPrimeChainOriginBitSizeAvg += workerConfig.SharesBitSize[i];
  bnPrimeChainOriginBitSizeAvg /= workerConfig.SharesBitSize.size();

  double d = log(Zmq::DivisionChecker_.prime(workerConfig.WeaveDepth));
  double primeProbBase = (1.78368 * d) / (bnPrimeChainOriginBitSizeAvg * log(2));
  double primeProbTarget = primeProbBase - (0.0003 * headerChainLength / 2);
  double primeProb7 = primeProbBase - (0.0003 * 7.0 / 2);
  double ppRatio = pow(primeProbTarget, headerChainLength) / pow(primeProb7, 7.0);
  double primeProb = headerChainLength != 7 ? pow(ppRatio, 1.0 / (headerChainLength - 7)) : 1.0;
  double shareValue = pow(primeProb / 0.1, headerChainLength - 7) * pow(primeProb, 7 - floor(shareTarget));
  return UInt<256>::fromDouble(shareValue * 4294967296.0);
}

uint32_t Zmq::Work::primePOWTarget()
{
  return TargetGetLength(Header.nBits);
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
