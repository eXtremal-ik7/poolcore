#include "api.h"
#include "p2p/p2p.h"
#include "poolcommon/poolapi.h"
#include "poolcore/backend.h"
#include "poolcommon/pool_generated.h"

void foundBlockHandler(asyncBase *base, p2pConnection *connection, const Query *Q, uint32_t id, PoolBackend *backend)
{
  flatbuffers::FlatBufferBuilder fbb;
  
  unsigned count = Q->count();
  auto &db = backend->accountingDb()->getFoundBlocksDb();
  std::unique_ptr<levelDbBase::IteratorType> It(db.iterator());
  if (Q->heightFrom() != -1) {
    foundBlock blk;
    blk.height = Q->heightFrom();
    blk.hash = Q->hashFrom() ? Q->hashFrom()->c_str() : "";
    It->seek(blk);
    It->prev();
  } else {
    It->seekLast();
  }
  
  std::vector<std::string> hashes;
  std::vector<int> confirmations;
  std::vector<foundBlock> foundBlocks;
  for (unsigned i = 0; i < Q->count() && It->valid(); i++) {
    foundBlock dbBlock;
    RawData data = It->value();
    if (!dbBlock.deserializeValue(data.data, data.size))
      break;
    foundBlocks.push_back(dbBlock);
    confirmations.push_back(-1);
    hashes.push_back(dbBlock.hash);
    It->prev();
  }
  
  // query confirmations
  auto result = ioGetBlockByHash(backend->client(), hashes);
  if (result && result->blocks.size() == hashes.size()) {
    for (size_t i = 0; i < result->blocks.size(); i++)
      confirmations[i] = result->blocks[i]->confirmations;
  }
  
  std::vector<flatbuffers::Offset<Block>> offsets;
  for (size_t i = 0; i < foundBlocks.size(); i++) {
    BlockT block;  
    block.height = foundBlocks[i].height;
    block.hash = foundBlocks[i].hash;
    block.time = foundBlocks[i].time;
    block.confirmations = confirmations[i];
    block.generatedCoins = foundBlocks[i].availableCoins;
    block.foundBy = foundBlocks[i].foundBy;
    offsets.push_back(CreateBlock(fbb, &block));
  }
  
  auto blockOffsets = fbb.CreateVector(offsets);
  QueryResultBuilder qrb(fbb);
  qrb.add_blocks(blockOffsets);
  fbb.Finish(qrb.Finish());
  aiop2pSend(base, connection, 3000000, fbb.GetBufferPointer(), p2pHeader(id, p2pMsgResponse, fbb.GetSize()), 0, 0);  
}

void poolBalanceHandler(asyncBase *base, p2pConnection *connection, const Query *Q, uint32_t id, AccountingDb *accountindDb)
{
  flatbuffers::FlatBufferBuilder fbb;
  
  unsigned count = Q->count();
  auto &db = accountindDb->getPoolBalanceDb();
  std::unique_ptr<levelDbBase::IteratorType> It(db.iterator());
  if (Q->timeFrom() != -1) {
    poolBalance pb;
    pb.time = Q->timeFrom();
    It->seek(pb);
    It->prev();
  } else {
    It->seekLast();
  }
  
  std::vector<flatbuffers::Offset<PoolBalance>> offsets;
  for (unsigned i = 0; i < count && It->valid(); i++) {
    poolBalance pb;
    RawData data = It->value();
    if (!pb.deserializeValue(data.data, data.size))
      break;
    
    offsets.push_back(CreatePoolBalance(fbb, pb.time, pb.balance, pb.immature, pb.users, pb.queued, pb.net));
    It->prev();
  }
  
  auto balanceOffsets = fbb.CreateVector(offsets);
  QueryResultBuilder qrb(fbb);
  qrb.add_poolBalances(balanceOffsets);
  fbb.Finish(qrb.Finish());
  aiop2pSend(base, connection, 3000000, fbb.GetBufferPointer(), p2pHeader(id, p2pMsgResponse, fbb.GetSize()), 0, 0);  
}

void payoutsHandler(asyncBase *base, p2pConnection *connection, const Query *Q, uint32_t id, AccountingDb *accountindDb)
{
  flatbuffers::FlatBufferBuilder fbb;
  
  unsigned count = Q->count();
  auto &db = accountindDb->getPayoutDb();
  std::unique_ptr<levelDbBase::IteratorType> It(db.iterator());

  {
    payoutRecord pr;
    pr.userId = Q->userId() ? Q->userId()->c_str() : "";
    pr.time = (Q->timeFrom() == -1) ? time(0)+1 : Q->timeFrom();
    It->seek(pr);
    It->prev();
  }
  
  std::vector<flatbuffers::Offset<PayoutRecord>> offsets;
  for (unsigned i = 0; i < count; i++) {
    if (!It->valid())
      break;
    
    payoutRecord pr;
    RawData data = It->value();
    if (!pr.deserializeValue(data.data, data.size) || pr.userId != Q->userId()->c_str())
      break;
    
    char timeLabel[128];
    tm *utc = gmtime(&pr.time);
    sprintf(timeLabel, "%04u-%02u-%02u %02u:%02u:%02u", utc->tm_year+1900, utc->tm_mon+1, utc->tm_mday, utc->tm_hour, utc->tm_min, utc->tm_sec);
    
    offsets.push_back(CreatePayoutRecord(fbb, fbb.CreateString(timeLabel), pr.time, pr.value, fbb.CreateString(pr.transactionId)));
    It->prev();
  }
  
  auto payoutOffsets = fbb.CreateVector(offsets);
  QueryResultBuilder qrb(fbb);
  qrb.add_payouts(payoutOffsets);
  fbb.Finish(qrb.Finish());
  aiop2pSend(base, connection, 3000000, fbb.GetBufferPointer(), p2pHeader(id, p2pMsgResponse, fbb.GetSize()), 0, 0);
}

void poolcoreRequestHandler(p2pPeer *peer, uint64_t id, void *buffer, size_t size, void *arg)
{
  PoolBackend *backend = (PoolBackend*)arg;
  
  flatbuffers::Verifier verifier((const uint8_t*)buffer, size);
  if (!VerifyP2PMessageBuffer(verifier)) {
    fprintf(stderr, "<error> can't decode query message\n");
    return;
  }
  
  const P2PMessage *msg = GetP2PMessage(buffer);
  
  auto Q = static_cast<const Query*>(msg->data());
  switch (msg->functionId()) {
    case FunctionId_QueryFoundBlocks :
      foundBlockHandler(peer->_base, peer->connection, Q, id, backend);
      break;
    case FunctionId_QueryClientInfo :
      backend->accountingDb()->queryClientBalance(peer, id, Q->userId()->c_str());
      break;
    case FunctionId_UpdateClientInfo : {
      const ClientInfo *info = Q->clientInfo();
      backend->accountingDb()->
        updateClientInfo(peer, id, Q->userId()->c_str(), info->name()->c_str(), info->email()->c_str(), info->minimalPayout());
      break;
    }
    case FunctionId_QueryPoolBalance :
      poolBalanceHandler(peer->_base, peer->connection, Q, id, backend->accountingDb());
      break;
    case FunctionId_QueryPayouts :
      payoutsHandler(peer->_base, peer->connection, Q, id, backend->accountingDb());
      break;
    case FunctionId_QueryClientStats :
      backend->statisticDb()->queryClientStats(peer, id, Q->userId()->c_str());
      break;
    case FunctionId_QueryPoolStats :
      backend->statisticDb()->queryPoolStats(peer, id);
      break;
    case FunctionId_ResendBrokenTx :
      backend->accountingDb()->resendBrokenTx(peer, id, Q->userId()->c_str());
      break;
    case FunctionId_MoveBalance :
      backend->accountingDb()->moveBalance(peer, id, Q->userId()->c_str(), Q->targetUserId()->c_str());
      break;
    case FunctionId_ManualPayout :
      backend->accountingDb()->manualPayout(peer, id, Q->userId()->c_str());
      break;
    default :
      fprintf(stderr, "<error> unknown query function id\n");
      break;
  }
}
