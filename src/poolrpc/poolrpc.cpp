#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "asyncio/socket.h"
#include "p2p/p2p.h"
#include "poolrpc/poolrpc.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

// TEMP
#include <unistd.h>

void getInfoHandler(p2pConnection *socket, const P2PMessage *msg, uint32_t id);
void getCurrentBlockHandler(p2pConnection *socket, const P2PMessage *msg, uint32_t id);


class PoolRpcObject {
private:
  enum pipeDescrs {
    Read = 0,
    Write
  };
  
private:
  int _pipeFd[2];
  BlockTemplateTy *_blockTemplate;
  ReserveKeyTy *_reserveKey;
  unsigned _extraNonce;
  
public:
  PoolRpcObject() : _blockTemplate(0), _reserveKey(0), _extraNonce(0) {
    pipe(_pipeFd);
  }
  
  int readFd() { return _pipeFd[Read]; }
  int writeFd() { return _pipeFd[Write]; }
  
  ReserveKeyTy *getReserveKey() { return _reserveKey; }
  
  void updateReserveKey() {
    _reserveKey = createReserveKey(getMainWallet());
  }
  
  BlockTemplateTy *getBlockTemplate() {
    return _blockTemplate;
  }
    
  BlockTemplateTy *generateBlockTemplate(unsigned *extraNonce) {
    mutateBlockTemplate(_blockTemplate, &_extraNonce);
    *extraNonce = _extraNonce;
    return _blockTemplate;
  }
  
  void updateBlockTemplate() {
    // deleteBlockTemplate(_blockTemplate);
    _blockTemplate = createBlockTemplate(_reserveKey);
    _extraNonce = 0;
  }
};

PoolRpcObject poolObject;

void newBlockNotify(void *index)
{
  // update block template
  poolObject.updateBlockTemplate();
  
  // serialize block using flatbuffers
  flatbuffers::FlatBufferBuilder fbb;

  BlockT block;
  createBlockRecord((BlockIndexTy*)index, block);
  auto blockOffset = CreateBlock(fbb, &block);

  fbb.Finish(CreateSignal(fbb, SignalId_NewBlock, Data_Block, blockOffset.Union()));
  
  uint32_t size = fbb.GetSize();
  write(poolObject.writeFd(), &size, sizeof(size));
  write(poolObject.writeFd(), fbb.GetBufferPointer(), size);
}

void getInfoHandler(asyncBase *base, p2pConnection *connection, const P2PMessage *msg, uint32_t id)
{
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(CreatePoolInfo(fbb,
    fbb.CreateString(getCoinName())
  ));
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void getCurrentBlockHandler(asyncBase *base, p2pConnection *connection, const P2PMessage *msg, uint32_t id)
{
  flatbuffers::FlatBufferBuilder fbb;
  BlockT block;
  createBlockRecord(getCurrentBlock(), block);
  fbb.Finish(CreateBlock(fbb, &block));
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void getBlockTemplateHandler(asyncBase *base, p2pConnection *connection, const P2PMessage *msg, uint32_t id)
{
  flatbuffers::FlatBufferBuilder fbb;

  BlockTemplateT block;
  if (!poolObject.getBlockTemplate())
    poolObject.updateBlockTemplate();
  
  unsigned extraNonce;
  BlockTemplateTy *tmpl = poolObject.generateBlockTemplate(&extraNonce);
  createBlockTemplateRecord(tmpl, extraNonce, block);
  fbb.Finish(CreateBlockTemplate(fbb, &block));
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void sendProofOfWorkHandler(asyncBase *base, p2pConnection *connection, const P2PMessage *msg, uint32_t id)
{
  flatbuffers::FlatBufferBuilder fbb;
  
  const ProofOfWorkReq *proofOfWork = static_cast<const ProofOfWorkReq*>(msg->data());
  auto data = proofOfWork->UnPack();

  int64_t generatedCoins;
  bool result = checkWork(poolObject.getBlockTemplate(), poolObject.getReserveKey(), *data, &generatedCoins);
  fbb.Finish(CreateProofOfWorkResult(fbb, result, generatedCoins));
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void getBlockByHashHandler(asyncBase *base, p2pConnection *connection, const P2PMessage *msg, uint32_t id)
{
  flatbuffers::FlatBufferBuilder fbb;
  const GetBlockByHashReq *hashes = static_cast<const GetBlockByHashReq*>(msg->data());
  std::vector<flatbuffers::Offset<Block>> offsets;
  for (size_t i = 0; i < hashes->hashes()->size(); i++) {
    BlockT block;
    BlockIndexTy *blockIndex = getBlockByHash(hashes->hashes()->Get(i)->c_str());
    createBlockRecord(blockIndex, block);
    offsets.push_back(CreateBlock(fbb, &block));
  }
  
  fbb.Finish(CreateGetBlockByHashResult(fbb, fbb.CreateVector(offsets)));
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void getBalanceHandler(asyncBase *base, p2pConnection *connection, const P2PMessage *msg, uint32_t id)
{
  flatbuffers::FlatBufferBuilder fbb;  
  GetBalanceResultT balance;
  getBalance(&balance);
  fbb.Finish(CreateGetBalanceResult(fbb, &balance));
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void sendMoneyHandler(asyncBase *base, p2pConnection *connection, const P2PMessage *msg, uint32_t id)
{
  flatbuffers::FlatBufferBuilder fbb;
  const SendMoneyReq *req = static_cast<const SendMoneyReq*>(msg->data());
  SendMoneyResultT result;
  sendMoney(req->destination()->c_str(), req->amount(), result);
  fbb.Finish(CreateSendMoneyResult(fbb, &result));
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void getZBalanceHandler(asyncBase *base, p2pConnection *connection, const P2PMessage *msg, uint32_t id)
{
  std::string error;
  flatbuffers::FlatBufferBuilder fbb;
  const WalletReq *req = static_cast<const WalletReq*>(msg->data());
  auto balance = ZGetbalance(req->singleAddress()->c_str(), error);
  
  auto errorOffset = fbb.CreateString(error);
  WalletResultBuilder wrb(fbb);
  wrb.add_balance(balance);
  wrb.add_error(errorOffset);
  fbb.Finish(wrb.Finish());
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void ZSendMoneyHandler(asyncBase *base, p2pConnection *connection, const P2PMessage *msg, uint32_t id)
{
  std::string error;  
  flatbuffers::FlatBufferBuilder fbb;
  const WalletReq *req = static_cast<const WalletReq*>(msg->data());
  
  std::vector<ZDestinationT> destinations;
  for (size_t i = 0; i < req->destinations()->size(); i++) {
    auto d = req->destinations()->Get(i)->UnPack();
    destinations.push_back(*d);
  }
  
  std::string asyncOpId = ZSendMoney(req->singleAddress()->c_str(), destinations, error);
  
  auto errorOffset = fbb.CreateString(error);  
  auto asyncOpIdOff = fbb.CreateString(asyncOpId);
  WalletResultBuilder wrb(fbb);
  wrb.add_asyncOperationId(asyncOpIdOff);
  wrb.add_error(errorOffset);  
  fbb.Finish(wrb.Finish());
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void listUnspentHandler(asyncBase *base, p2pConnection *connection, const P2PMessage *msg, uint32_t id)
{
  std::string error;  
  flatbuffers::FlatBufferBuilder fbb;
  std::vector<ListUnspentElementT> out;
  std::vector<flatbuffers::Offset<ListUnspentElement>> offsets;
  
  listUnspent(out, error);
  for (size_t i = 0; i < out.size(); i++)
    offsets.push_back(CreateListUnspentElement(fbb, &out[i]));
  
  auto errorOffset = fbb.CreateString(error);  
  auto outVector = fbb.CreateVector(offsets);
  WalletResultBuilder wrb(fbb);
  wrb.add_outs(outVector);
  wrb.add_error(errorOffset);  
  fbb.Finish(wrb.Finish());
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void ZAsyncOperationStatusHandler(asyncBase *base, p2pConnection *connection, const P2PMessage *msg, uint32_t id)
{
  std::string error;  
  flatbuffers::FlatBufferBuilder fbb;
  const WalletReq *req = static_cast<const WalletReq*>(msg->data());  
  std::vector<AsyncOperationStatusT> out;
  std::vector<flatbuffers::Offset<AsyncOperationStatus>> offsets;
  
  ZAsyncOperationStatus(req->singleAsyncOperationId() ? req->singleAsyncOperationId()->c_str() : "",
                        out,
                        error);
  for (size_t i = 0; i < out.size(); i++)
    offsets.push_back(CreateAsyncOperationStatus(fbb, &out[i]));
  
  auto errorOffset = fbb.CreateString(error);  
  auto outVector = fbb.CreateVector(offsets);
  WalletResultBuilder wrb(fbb);
  wrb.add_status(outVector);
  wrb.add_error(errorOffset);  
  fbb.Finish(wrb.Finish());
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}


void requestHandler(p2pPeer *peer, uint32_t id, void *buffer, size_t size, void *arg)
{
  flatbuffers::Verifier verifier((const uint8_t*)buffer, size);
  if (!VerifyP2PMessageBuffer(verifier)) {
    printf(" * poolrpc error: can't decode message\n");
    return;
  }
      
  const P2PMessage *msg = GetP2PMessage(buffer);
     
  switch (msg->functionId()) {
    case FunctionId_GetInfo :
      getInfoHandler(peer->_base, peer->connection, msg, id);
      break;
    case FunctionId_GetCurrentBlock :
      getCurrentBlockHandler(peer->_base, peer->connection, msg, id);
      break;
    case FunctionId_GetBlockTemplate :
      getBlockTemplateHandler(peer->_base, peer->connection, msg, id);
      break;
    case FunctionId_SendProofOfWork :
      sendProofOfWorkHandler(peer->_base, peer->connection, msg, id);
      break;
    case FunctionId_GetBlockByHash :
      getBlockByHashHandler(peer->_base, peer->connection, msg, id);
      break;
    case FunctionId_GetBalance :
      getBalanceHandler(peer->_base, peer->connection, msg, id);
      break;
    case FunctionId_SendMoney :
      sendMoneyHandler(peer->_base, peer->connection, msg, id);
      break;
    case FunctionId_ZGetBalance :
      getZBalanceHandler(peer->_base, peer->connection, msg, id);
      break;
    case FunctionId_ZSendMoney :
      ZSendMoneyHandler(peer->_base, peer->connection, msg, id);
      break;
    case FunctionId_ListUnspent :
      listUnspentHandler(peer->_base, peer->connection, msg, id);
      break;
    case FunctionId_ZAsyncOperationStatus :
      ZAsyncOperationStatusHandler(peer->_base, peer->connection, msg, id);
      break;
      break;
    default :
      printf(" * poolrpc error: unknown function id\n");
      break;
  }
}

void signalProc(void *arg)
{
  p2pNode *node = (p2pNode*)arg;
  aioObject *object = newDeviceIo(node->base(), poolObject.readFd());
  xmstream stream;
  while (true) {
    uint32_t msgSize;
    stream.reset();
    if (ioRead(object, &msgSize, sizeof(msgSize), afWaitAll, 0) != sizeof(msgSize))
      break;
    if (ioRead(object, stream.alloc(msgSize), msgSize, afWaitAll, 0) != msgSize)
      break;
    stream.seekSet(0);
            
    const Signal *signal = flatbuffers::GetRoot<Signal>(stream.data()); 
    switch (signal->signalId()) {
      case SignalId_NewBlock : {
        node->sendSignal(stream.data(), stream.sizeOf());
        break;
      }
      default :
        break;
    }
  }
}

void *poolRpcThread(void *arg)
{
  initializeSocketSubsystem();  
  poolObject.updateReserveKey();  
  asyncBase *base = createAsyncBase(amOSDefault);  
  
  // // TODO: cluster name must contain coin name, poolrpc not valid
  uint16_t port = static_cast<uint16_t>(p2pPort());
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = xhton<uint16_t>(port);    
  p2pNode *node = p2pNode::createNode(base, &address, "pool_rpc", true);
  if (!node) {
    printf("can't create poolrpc node\n");
    return nullptr;
  }
  
  printf("started p2p interface at port %i\n", port);
  node->setRequestHandler(requestHandler, nullptr);
  
  // run signals check coroutine
  coroutineTy *signalHandler = coroutineNew(signalProc, node, 0x10000);
  coroutineCall(signalHandler);
  
  asyncLoop(base);
  return nullptr;
}
