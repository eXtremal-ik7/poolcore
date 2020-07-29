#pragma once

#include "blockmaker/btc.h"
#include "blockmaker/merkleTree.h"
#include "poolcore/backend.h"
#include "rapidjson/document.h"
#include "loguru.hpp"
#include "asyncio/socket.h"

struct CExtraNonce {
  uint64_t data[2];
};

template<typename Proto>
class CSingleWorkInstance : public CWorkInstance {
public:
  typename Proto::Block Block;
  uint64_t Height;
  size_t ScriptSigExtraNonceOffset;
};


using ListenerCallback = std::function<void(socketTy, HostAddress, void*)>;

struct ListenerContext {
   ListenerCallback Callback;
  void *Arg;
};

static void listenerAcceptCb(AsyncOpStatus status, aioObject *object, HostAddress address, socketTy socket, void *arg)
{
  if (status == aosSuccess) {
    ListenerContext *ctx = static_cast<ListenerContext*>(arg);
    ctx->Callback(socket, address, ctx->Arg);
  }

  aioAccept(object, 0, listenerAcceptCb, arg);
}

static void createListener(asyncBase *base, uint16_t port, ListenerCallback callback, void *arg)
{
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = htons(port);
  socketTy hSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  socketReuseAddr(hSocket);
  if (socketBind(hSocket, &address) != 0) {
    LOG_F(ERROR, "cannot bind port: %i", port);
    exit(1);
  }

  if (socketListen(hSocket) != 0) {
    LOG_F(ERROR, "listen error: %i", port);
    exit(1);
  }

  aioObject *object = newSocketIo(base, hSocket);

  ListenerContext *context = new ListenerContext;
  context->Callback = callback;
  context->Arg = arg;
  aioAccept(object, 0, listenerAcceptCb, context);
}

template<typename Proto>
CSingleWorkInstance<Proto> *acceptBlockTemplate(rapidjson::Value &blockTemplate,
                                                const PoolBackendConfig &cfg,
                                                const CCoinInfo &coinInfo,
                                                xmstream &serializeBuffer,
                                                const std::string &instanceName,
                                                size_t extraNonceSize)
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
    LOG_F(WARNING, "%s: mining address %s is invalid", coinInfo.Name.c_str(), cfg.MiningAddress.c_str());
    return nullptr;
  }

  bool coinBaseSuccess = coinInfo.SegwitEnabled ?
    buildSegwitCoinbaseFromTemplate(work->Block.vtx[0], miningAddress, cfg.CoinBaseMsg, blockTemplate, extraNonceSize, &work->ScriptSigExtraNonceOffset) :
    buildCoinbaseFromTemplate(work->Block.vtx[0], miningAddress, cfg.CoinBaseMsg, blockTemplate, extraNonceSize, &work->ScriptSigExtraNonceOffset);
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
