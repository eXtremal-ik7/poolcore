#include "api.h"
#include "p2p/p2p.h"
#include "poolcommon/poolapi.h"
#include "poolcore/backend.h"
#include "poolcommon/pool_generated.h"
#include "loguru.hpp"

void foundBlockHandler(asyncBase *base, p2pConnection *connection, const Query *Q, uint32_t id, PoolBackend *backend)
{
  flatbuffers::FlatBufferBuilder fbb;
  
  auto &db = backend->accountingDb()->getFoundBlocksDb();
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());
  if (Q->heightFrom() != -1) {
    FoundBlockRecord blk;
    blk.Height = Q->heightFrom();
    blk.Hash = Q->hashFrom() ? Q->hashFrom()->c_str() : "";
    It->seek(blk);
    It->prev();
  } else {
    It->seekLast();
  }
  
  std::vector<std::string> hashes;
  std::vector<int> confirmations;
  std::vector<FoundBlockRecord> foundBlocks;
  for (unsigned i = 0; i < Q->count() && It->valid(); i++) {
    FoundBlockRecord dbBlock;
    RawData data = It->value();
    if (!dbBlock.deserializeValue(data.data, data.size))
      break;
    foundBlocks.push_back(dbBlock);
    confirmations.push_back(-1);
    hashes.push_back(dbBlock.Hash);
    It->prev();
  }
  
  // query confirmations
  if (Q->count()) {
    auto result = ioGetBlockByHash(backend->client(), hashes);
    if (result && result->blocks.size() == hashes.size()) {
      for (size_t i = 0; i < result->blocks.size(); i++)
        confirmations[i] = result->blocks[i]->confirmations;
    }
  }
  
  std::vector<flatbuffers::Offset<Block>> offsets;
  for (size_t i = 0; i < foundBlocks.size(); i++) {
    BlockT block;  
    block.height = foundBlocks[i].Height;
    block.hash = foundBlocks[i].Hash;
    block.time = foundBlocks[i].Time;
    block.confirmations = confirmations[i];
    block.generatedCoins = foundBlocks[i].AvailableCoins;
    block.foundBy = foundBlocks[i].FoundBy;
    offsets.push_back(CreateBlock(fbb, &block));
  }
  
  auto blockOffsets = fbb.CreateVector(offsets);
  QueryResultBuilder qrb(fbb);
  qrb.add_blocks(blockOffsets);
  fbb.Finish(qrb.Finish());
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void poolBalanceHandler(asyncBase *base, p2pConnection *connection, const Query *Q, uint32_t id, AccountingDb *accountindDb)
{
  flatbuffers::FlatBufferBuilder fbb;
  
  unsigned count = Q->count();
  auto &db = accountindDb->getPoolBalanceDb();
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());
  if (Q->timeFrom() != -1) {
    PoolBalanceRecord pb;
    pb.Time = Q->timeFrom();
    It->seek(pb);
    It->prev();
  } else {
    It->seekLast();
  }
  
  std::vector<flatbuffers::Offset<PoolBalance>> offsets;
  for (unsigned i = 0; i < count && It->valid(); i++) {
    PoolBalanceRecord pb;
    RawData data = It->value();
    if (!pb.deserializeValue(data.data, data.size))
      break;
    
    offsets.push_back(CreatePoolBalance(fbb, pb.Time, pb.Balance, pb.Immature, pb.Users, pb.Queued, pb.Net));
    It->prev();
  }
  
  auto balanceOffsets = fbb.CreateVector(offsets);
  QueryResultBuilder qrb(fbb);
  qrb.add_poolBalances(balanceOffsets);
  fbb.Finish(qrb.Finish());
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void payoutsHandler(asyncBase *base, p2pConnection *connection, const Query *Q, uint32_t id, AccountingDb *accountindDb)
{
  flatbuffers::FlatBufferBuilder fbb;
  
  unsigned count = Q->count();
  auto &db = accountindDb->getPayoutDb();
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());

  {
    PayoutDbRecord pr;
    pr.userId = Q->userId() ? Q->userId()->c_str() : "";
    pr.time = (Q->timeFrom() == -1) ? time(nullptr)+1 : Q->timeFrom();
    It->seek(pr);
    It->prev();
  }
  
  std::vector<flatbuffers::Offset<PayoutRecord>> offsets;
  for (unsigned i = 0; i < count; i++) {
    if (!It->valid())
      break;
    
    PayoutDbRecord pr;
    RawData data = It->value();
    if (!pr.deserializeValue(data.data, data.size) || pr.userId != Q->userId()->c_str())
      break;
    
    char timeLabel[128];
    tm *utc = gmtime(&pr.time);
    sprintf(timeLabel, "%04u-%02u-%02u %02u:%02u:%02u", utc->tm_year+1900, utc->tm_mon+1, utc->tm_mday, utc->tm_hour, utc->tm_min, utc->tm_sec);
    
    offsets.push_back(CreatePayoutRecord(fbb, fbb.CreateString((const char*)timeLabel), pr.time, pr.value, fbb.CreateString(pr.transactionId)));
    It->prev();
  }
  
  auto payoutOffsets = fbb.CreateVector(offsets);
  QueryResultBuilder qrb(fbb);
  qrb.add_payouts(payoutOffsets);
  fbb.Finish(qrb.Finish());
  aiop2pSend(connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void poolcoreRequestHandler(p2pPeer *peer, uint32_t id, void *buffer, size_t size, void *arg)
{
  PoolBackend *backend = (PoolBackend*)arg;
  
  flatbuffers::Verifier verifier((const uint8_t*)buffer, size);
  if (!VerifyP2PMessageBuffer(verifier)) {
    LOG_F(ERROR, "can't decode query message");
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
      LOG_F(ERROR, "<error> unknown query function id");
      break;
  }
}
