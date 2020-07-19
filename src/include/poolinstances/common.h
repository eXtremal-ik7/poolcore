#pragma once

#include "blockmaker/btc.h"
#include "poolcore/backend.h"
#include "rapidjson/document.h"
#include "loguru.hpp"

template<typename Proto>
class CSingleWorkInstance : public CWorkInstance {
public:
  typename Proto::Block block;
  size_t extraNonceOffset;
};

template<typename Proto>
CSingleWorkInstance<Proto> *checkNewBlockTemplate(rapidjson::Value &blockTemplate, const PoolBackendConfig &cfg, const CCoinInfo &coinInfo, xmstream &serializeBuffer, const std::string &instanceName)
{
  std::unique_ptr<CSingleWorkInstance<Proto>> work(new CSingleWorkInstance<Proto>);

  // Fill block with header fields and transactions
  // Header fields
  if (!Proto::loadHeaderFromTemplate(work->block.header, blockTemplate)) {
    LOG_F(WARNING, "%s: Malformed block template (can't parse block header)", instanceName.c_str());
    return nullptr;
  }

  // Transactions
  if (!loadTransactionsFromTemplate(work->block.vtx, blockTemplate, serializeBuffer)) {
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
    buildSegwitCoinbaseFromTemplate(work->block.vtx[0], miningAddress, blockTemplate) :
    buildCoinbaseFromTemplate(work->block.vtx[0], miningAddress, cfg.CoinBaseMsg, blockTemplate, &work->extraNonceOffset);
  if (!coinBaseSuccess) {
    LOG_F(WARNING, "%s: Insufficient data for coinbase transaction in block template", instanceName.c_str());
    return nullptr;
  }

  return work.release();
}
