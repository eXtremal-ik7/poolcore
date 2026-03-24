#pragma once

#include "poolcore/blockTemplate.h"
#include "poolcore/ethereumRpc.idl.h"

struct CEthereumBlockTemplate : CBlockTemplate {
  CEthereumBlockTemplate(const std::string &ticker, EWorkType workType) : CBlockTemplate(ticker, workType) {}
  CETHTemplate Data;
};
