#pragma once

#include "poolcore/blockTemplate.h"
#include "poolcore/bitcoinRpc.idl.h"

struct CBitcoinBlockTemplate : CBlockTemplate {
  CBitcoinBlockTemplate(const std::string &ticker, EWorkType workType) : CBlockTemplate(ticker, workType) {}
  CBTCTemplate Data;
};
