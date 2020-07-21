#pragma once

#include "blockmaker/btc.h"
#include "blockmaker/merkleTree.h"
#include "poolcore/backend.h"
#include "rapidjson/document.h"
#include "loguru.hpp"

struct CExtraNonce {
  uint64_t data[2];
};

template<typename Proto>
class CSingleWorkInstance : public CWorkInstance {
public:
  typename Proto::Block Block;
  uint64_t Height;
  size_t ExtraNonceOffset;
};

template<typename Proto>
CSingleWorkInstance<Proto> *checkNewBlockTemplate(rapidjson::Value &blockTemplate, const PoolBackendConfig &cfg, const CCoinInfo &coinInfo, xmstream &serializeBuffer, const std::string &instanceName)
{
  std::unique_ptr<CSingleWorkInstance<Proto>> work(new CSingleWorkInstance<Proto>);

  // Height
  if (!blockTemplate.HasMember("height") || !blockTemplate["height"].IsUint64()) {
    LOG_F(WARNING, "%s: block template does not contains 'height' field", instanceName.c_str());
    return nullptr;
  }
  work->Height = blockTemplate["height"].GetUint64();

  // Fill block with header fields and transactions
  // Header fields
  if (!Proto::loadHeaderFromTemplate(work->Block.header, blockTemplate)) {
    LOG_F(WARNING, "%s: Malformed block template (can't parse block header)", instanceName.c_str());
    return nullptr;
  }

  // Transactions
  if (!loadTransactionsFromTemplate(work->Block.vtx, blockTemplate, serializeBuffer)) {
    LOG_F(WARNING, "%s: Malformed block template (can't parse transactions)", instanceName.c_str());
    return nullptr;
  }

  // Build coinbase
  typename Proto::AddressTy miningAddress;
  if (!decodeHumanReadableAddress(cfg.MiningAddress, coinInfo.PubkeyAddressPrefix, miningAddress)) {
    LOG_F(WARNING, "%s: mining address %s is invalid", cfg.MiningAddress.c_str());
    return nullptr;
  }

  bool coinBaseSuccess = coinInfo.SegwitEnabled ?
    buildSegwitCoinbaseFromTemplate(work->Block.vtx[0], miningAddress, blockTemplate) :
    buildCoinbaseFromTemplate(work->Block.vtx[0], miningAddress, cfg.CoinBaseMsg, blockTemplate, &work->ExtraNonceOffset);
  if (!coinBaseSuccess) {
    LOG_F(WARNING, "%s: Insufficient data for coinbase transaction in block template", instanceName.c_str());
    return nullptr;
  }

  return work.release();
}

template<typename Proto>
void initializeExtraNonce(typename Proto::Block &block, size_t extraNonceOffset, unsigned workerId) {
  typename Proto::TxIn txIn = block.vtx[0].txIn[0];
  uint8_t *txInData = txIn.scriptSig.data();
  uint64_t *extraNonce = reinterpret_cast<uint64_t*>(txInData + extraNonceOffset);
  extraNonce[0] = workerId;
  extraNonce[1] = 0;
  // Update merkle tree
  block.vtx[0].Hash.SetNull();
  block.header.hashMerkleRoot = calculateMerkleRoot<Proto>(block.vtx);
}

template<typename Proto>
void incrementExtraNonce(typename Proto::Block &block, size_t extraNonceOffset, unsigned workersNum, CExtraNonce &out) {
  typename Proto::TxIn txIn = block.vtx[0].txIn[0];
  uint8_t *txInData = txIn.scriptSig.data();
  uint64_t *extraNonce = reinterpret_cast<uint64_t*>(txInData + extraNonceOffset);
  uint64_t oldValue = extraNonce[0];
  // uint128_t addition
  extraNonce[0] += workersNum;
  if (extraNonce[0] < oldValue)
    extraNonce[1]++;
  out.data[0] = extraNonce[0];
  out.data[1] = extraNonce[1];
  // Update merkle tree
  block.vtx[0].Hash.SetNull();
  block.header.hashMerkleRoot = calculateMerkleRoot<Proto>(block.vtx);
}


static inline uint8_t hexLowerCaseDigit2bin(char c)
{
  uint8_t digit = c - '0';
  if (digit >= 10)
    digit -= ('a' - '0' - 10);
  return digit;
}

static inline const char bin2hexLowerCaseDigit(uint8_t b)
{
  return b < 10 ? '0'+b : 'a'+b-10;
}

static inline void hexLowerCase2bin(const char *in, size_t inSize, void *out)
{
  uint8_t *pOut = static_cast<uint8_t*>(out);
  for (size_t i = 0; i < inSize/2; i++)
    pOut[i] = (hexLowerCaseDigit2bin(in[i*2]) << 4) | hexLowerCaseDigit2bin(in[i*2+1]);
}

static inline void bin2hexLowerCase(const void *in, char *out, size_t size)
{
  const uint8_t *pIn = static_cast<const uint8_t*>(in);
  for (size_t i = 0, ie = size; i != ie; ++i) {
    out[i*2] = bin2hexLowerCaseDigit(pIn[i] >> 4);
    out[i*2+1] = bin2hexLowerCaseDigit(pIn[i] & 0xF);
  }
}
