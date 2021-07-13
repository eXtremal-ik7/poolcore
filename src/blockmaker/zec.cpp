#include "blockmaker/zec.h"
#include "poolcommon/arith_uint256.h"
#include "sodium/crypto_generichash_blake2b.h"

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

inline constexpr size_t equihash_solution_size(unsigned int N, unsigned int K) {
    return (1 << K)*(N/(K+1)+1)/8;
}

typedef uint32_t eh_index;
typedef uint8_t eh_trunc;
typedef crypto_generichash_blake2b_state eh_HashState;

static void EhIndexToArray(const eh_index i, unsigned char* array)
{
    eh_index bei = htobe32(i);
    memcpy(array, &bei, sizeof(eh_index));
}

static void ExpandArray(const unsigned char* in, size_t in_len, unsigned char* out, size_t out_len, size_t bit_len, size_t byte_pad = 0)
{
    assert(bit_len >= 8);
    assert(8*sizeof(uint32_t) >= 7+bit_len);

    size_t out_width { (bit_len+7)/8 + byte_pad };
    assert(out_len == 8*out_width*in_len/bit_len);

    uint32_t bit_len_mask { ((uint32_t)1 << bit_len) - 1 };

    // The acc_bits least-significant bits of acc_value represent a bit sequence
    // in big-endian order.
    size_t acc_bits = 0;
    uint32_t acc_value = 0;

    size_t j = 0;
    for (size_t i = 0; i < in_len; i++) {
        acc_value = (acc_value << 8) | in[i];
        acc_bits += 8;

        // When we have bit_len or more bits in the accumulator, write the next
        // output element.
        if (acc_bits >= bit_len) {
            acc_bits -= bit_len;
            for (size_t x = 0; x < byte_pad; x++) {
                out[j+x] = 0;
            }
            for (size_t x = byte_pad; x < out_width; x++) {
                out[j+x] = (
                    // Big-endian
                    acc_value >> (acc_bits+(8*(out_width-x-1)))
                ) & (
                    // Apply bit_len_mask across byte boundaries
                    (bit_len_mask >> (8*(out_width-x-1))) & 0xFF
                );
            }
            j += out_width;
        }
    }
}


template<size_t WIDTH>
class StepRow
{
    template<size_t W>
    friend class StepRow;
    friend class CompareSR;

protected:
    unsigned char hash[WIDTH];

public:
    StepRow(const unsigned char* hashIn, size_t hInLen, size_t hLen, size_t cBitLen) {
      assert(hLen <= WIDTH);
      ExpandArray(hashIn, hInLen, hash, hLen, cBitLen);
    }
    ~StepRow() { }

    template<size_t W>
    StepRow(const StepRow<W>& a);

    bool IsZero(size_t len) {
      // This doesn't need to be constant time.
      for (size_t i = 0; i < len; i++) {
          if (hash[i] != 0)
              return false;
      }
      return true;
    }
    std::string GetHex(size_t len) { return HexStr(hash, hash+len); }

    template<size_t W>
    friend bool HasCollision(StepRow<W>& a, StepRow<W>& b, int l);
};

template<size_t WIDTH>
class FullStepRow : public StepRow<WIDTH>
{
    template<size_t W>
    friend class FullStepRow;

    using StepRow<WIDTH>::hash;

public:
    FullStepRow(const unsigned char* hashIn, size_t hInLen, size_t hLen, size_t cBitLen, eh_index i) : StepRow<WIDTH> {hashIn, hInLen, hLen, cBitLen} {
      EhIndexToArray(i, hash+hLen);
    }
    ~FullStepRow() { }

    FullStepRow(const FullStepRow<WIDTH>& a) : StepRow<WIDTH> {a} { }
    template<size_t W>
    FullStepRow(const FullStepRow<W>& a, const FullStepRow<W>& b, size_t len, size_t lenIndices, int trim) : StepRow<WIDTH> {a} {
      assert(len+lenIndices <= W);
      assert(len-trim+(2*lenIndices) <= WIDTH);
      for (size_t i = trim; i < len; i++)
          hash[i-trim] = a.hash[i] ^ b.hash[i];
      if (a.IndicesBefore(b, len, lenIndices)) {
          std::copy(a.hash+len, a.hash+len+lenIndices, hash+len-trim);
          std::copy(b.hash+len, b.hash+len+lenIndices, hash+len-trim+lenIndices);
      } else {
          std::copy(b.hash+len, b.hash+len+lenIndices, hash+len-trim);
          std::copy(a.hash+len, a.hash+len+lenIndices, hash+len-trim+lenIndices);
      }
    }
    FullStepRow& operator=(const FullStepRow<WIDTH>& a) {
      std::copy(a.hash, a.hash+WIDTH, hash);
      return *this;
    }

    inline bool IndicesBefore(const FullStepRow<WIDTH>& a, size_t len, size_t lenIndices) const { return memcmp(hash+len, a.hash+len, lenIndices) < 0; }
    std::vector<unsigned char> GetIndices(size_t len, size_t lenIndices,
                                          size_t cBitLen) const;

    template<size_t W>
    friend bool DistinctIndices(const FullStepRow<W>& a, const FullStepRow<W>& b,
                                size_t len, size_t lenIndices);
    template<size_t W>
    friend bool IsValidBranch(const FullStepRow<W>& a, const size_t len, const unsigned int ilen, const eh_trunc t);
};

template<size_t WIDTH>
bool DistinctIndices(const FullStepRow<WIDTH>& a, const FullStepRow<WIDTH>& b, size_t len, size_t lenIndices)
{
    for(size_t i = 0; i < lenIndices; i += sizeof(eh_index)) {
        for(size_t j = 0; j < lenIndices; j += sizeof(eh_index)) {
            if (memcmp(a.hash+len+i, b.hash+len+j, sizeof(eh_index)) == 0) {
                return false;
            }
        }
    }
    return true;
}

inline constexpr  size_t max(const size_t A, const size_t B) { return A > B ? A : B; }

static eh_index ArrayToEhIndex(const unsigned char* array)
{
    eh_index bei;
    memcpy(&bei, array, sizeof(eh_index));
    return be32toh(bei);
}

static void GenerateHash(const eh_HashState& base_state, eh_index g, unsigned char* hash, size_t hLen)
{
    eh_HashState state;
    state = base_state;
    eh_index lei = htole32(g);
    crypto_generichash_blake2b_update(&state, (const unsigned char*) &lei,
                                      sizeof(eh_index));
    crypto_generichash_blake2b_final(&state, hash, hLen);
}

static std::vector<eh_index> GetIndicesFromMinimal(std::vector<unsigned char> minimal, size_t cBitLen)
{
    assert(((cBitLen+1)+7)/8 <= sizeof(eh_index));
    size_t lenIndices { 8*sizeof(eh_index)*minimal.size()/(cBitLen+1) };
    size_t bytePad { sizeof(eh_index) - ((cBitLen+1)+7)/8 };
    std::vector<unsigned char> array(lenIndices);
    ExpandArray(minimal.data(), minimal.size(),
                array.data(), lenIndices, cBitLen+1, bytePad);
    std::vector<eh_index> ret;
    for (size_t i = 0; i < lenIndices; i += sizeof(eh_index)) {
        ret.push_back(ArrayToEhIndex(array.data()+i));
    }
    return ret;
}

template<size_t WIDTH>
bool HasCollision(StepRow<WIDTH>& a, StepRow<WIDTH>& b, int l)
{
    // This doesn't need to be constant time.
    for (int j = 0; j < l; j++) {
        if (a.hash[j] != b.hash[j])
            return false;
    }
    return true;
}

template<unsigned int N, unsigned int K>
class Equihash
{
public:
    enum : size_t { IndicesPerHashOutput=512/N };
    enum : size_t { HashOutput=IndicesPerHashOutput*N/8 };
    enum : size_t { CollisionBitLength=N/(K+1) };
    enum : size_t { CollisionByteLength=(CollisionBitLength+7)/8 };
    enum : size_t { HashLength=(K+1)*CollisionByteLength };
    enum : size_t { FullWidth=2*CollisionByteLength+sizeof(eh_index)*(1 << (K-1)) };
    enum : size_t { FinalFullWidth=2*CollisionByteLength+sizeof(eh_index)*(1 << (K)) };
    enum : size_t { TruncatedWidth=max(HashLength+sizeof(eh_trunc), 2*CollisionByteLength+sizeof(eh_trunc)*(1 << (K-1))) };
    enum : size_t { FinalTruncatedWidth=max(HashLength+sizeof(eh_trunc), 2*CollisionByteLength+sizeof(eh_trunc)*(1 << (K))) };
    enum : size_t { SolutionWidth=(1 << K)*(CollisionBitLength+1)/8 };

    Equihash() { }

    int InitialiseState(eh_HashState& base_state) {
      uint32_t le_N = htole32(N);
      uint32_t le_K = htole32(K);
      unsigned char personalization[crypto_generichash_blake2b_PERSONALBYTES] = {};
      memcpy(personalization, "ZcashPoW", 8);
      memcpy(personalization+8,  &le_N, 4);
      memcpy(personalization+12, &le_K, 4);
      return crypto_generichash_blake2b_init_salt_personal(&base_state,
                                                           NULL, 0, // No key.
                                                           (512/N)*N/8,
                                                           NULL,    // No salt.
                                                           personalization);
    }

    bool IsValidSolution(const eh_HashState& base_state, std::vector<unsigned char> &soln) {
      if (soln.size() != SolutionWidth) {
          return false;
      }

      std::vector<FullStepRow<FinalFullWidth>> X;
      X.reserve(1 << K);
      unsigned char tmpHash[HashOutput];
      for (eh_index i : GetIndicesFromMinimal(soln, CollisionBitLength)) {
          GenerateHash(base_state, i/IndicesPerHashOutput, tmpHash, HashOutput);
          X.emplace_back(tmpHash+((i % IndicesPerHashOutput) * N/8), N/8, HashLength, CollisionBitLength, i);
      }

      size_t hashLen = HashLength;
      size_t lenIndices = sizeof(eh_index);
      while (X.size() > 1) {
          std::vector<FullStepRow<FinalFullWidth>> Xc;
          for (size_t i = 0; i < X.size(); i += 2) {
              if (!HasCollision(X[i], X[i+1], CollisionByteLength)) {
                  return false;
              }
              if (X[i+1].IndicesBefore(X[i], hashLen, lenIndices)) {
                  return false;
              }
              if (!DistinctIndices(X[i], X[i+1], hashLen, lenIndices)) {
                  return false;
              }
              Xc.emplace_back(X[i], X[i+1], hashLen, lenIndices, CollisionByteLength);
          }
          X = Xc;
          hashLen -= CollisionByteLength;
          lenIndices *= 2;
      }

      assert(X.size() == 1);
      return X[0].IsZero(hashLen);
    }
};

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

bool Proto::checkConsensus(const ZEC::Proto::BlockHeader &header, CheckConsensusCtx &consensusCtx, ZEC::Proto::ChainParams&, double *shareDiff)
{
  static Equihash<200,9> Eh200_9;
  static Equihash<48,5> Eh48_5;

  // Check equihash solution
  // Here used original code from ZCash repository, it causes high CPU load
  // TODO: optimize it
  *shareDiff = 0;
  crypto_generichash_blake2b_state state;
  if (consensusCtx.N == 200 && consensusCtx.K == 9) {
    Eh200_9.InitialiseState(state);
    crypto_generichash_blake2b_update(&state, reinterpret_cast<const uint8_t*>(&header), 4+32+32+32+4+4);
    crypto_generichash_blake2b_update(&state, (const uint8_t*)header.nNonce.begin(), 32);
    std::vector<uint8_t> proofForCheck(header.nSolution.begin(), header.nSolution.end());
    if (!Eh200_9.IsValidSolution(state, proofForCheck))
      return false;
  } else if (consensusCtx.N == 48 && consensusCtx.K == 5) {
    Eh48_5.InitialiseState(state);
    crypto_generichash_blake2b_update(&state, reinterpret_cast<const uint8_t*>(&header), 4+32+32+32+4+4);
    crypto_generichash_blake2b_update(&state, (const uint8_t*)header.nNonce.begin(), 32);
    std::vector<uint8_t> proofForCheck(header.nSolution.begin(), header.nSolution.end());
    if (!Eh48_5.IsValidSolution(state, proofForCheck))
      return false;
  } else {
    return false;
  }

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
      !blockTemplate.HasMember("curtime") ||
      !blockTemplate.HasMember("bits")) {
    return false;
  }

  rapidjson::Value &version = blockTemplate["version"];
  rapidjson::Value &hashPrevBlock = blockTemplate["previousblockhash"];
  rapidjson::Value &curtime = blockTemplate["curtime"];
  rapidjson::Value &bits = blockTemplate["bits"];

  header.nVersion = version.GetUint();
  header.hashPrevBlock.SetHex(hashPrevBlock.GetString());
  header.hashMerkleRoot = calculateMerkleRoot(legacy.Data.data(), legacy.Data.sizeOf(), merklePath);
  header.nTime = curtime.GetUint();
  header.nBits = strtoul(bits.GetString(), nullptr, 16);
  header.nNonce.SetNull();
  header.nSolution.resize(0);

  if (blockTemplate.HasMember("finalsaplingroothash") && blockTemplate["finalsaplingroothash"].IsString())
    header.hashLightClientRoot.SetHex(blockTemplate["finalsaplingroothash"].GetString());
  else
    header.hashLightClientRoot.SetNull();

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

