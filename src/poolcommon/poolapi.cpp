#include "poolcommon/poolapi.h"
#include "p2p/p2p.h"
#include "loguru.hpp"

template<typename T>
static inline const T *decodeAs(p2pPeer *peer, const char *name)
{
  p2pStream &stream = peer->connection->stream;
  flatbuffers::Verifier verifier((const uint8_t*)stream.data(), stream.sizeOf());
  if (!verifier.VerifyBuffer<T>(nullptr)) {
    LOG_F(ERROR, "poolapi: '%s' can't decode response", name);
    return 0;
  }
  return flatbuffers::GetRoot<T>(stream.data());
}

template<typename T>
static inline const T *request(p2pNode *client,
                               flatbuffers::FlatBufferBuilder &fbb,
                               uint64_t timeout,
                               const char *name,
                               void *out,
                               size_t outSize)
{
  if (client->ioRequest(fbb.GetBufferPointer(), fbb.GetSize(), timeout, out, outSize)) {
    flatbuffers::Verifier verifier((const uint8_t*)out, outSize);
    if (!verifier.VerifyBuffer<T>(nullptr)) {
      LOG_F(ERROR, "poolapi: '%s' can't decode response", name);
      return 0;
    }
    
    return flatbuffers::GetRoot<T>(out);
  } else {
    LOG_F(ERROR, "poolapi: '%s' call failed", name);
    return 0;
  }
}

std::unique_ptr<PoolInfoT> ioGetInfo(p2pNode *client)
{
  uint8_t buffer[1024];
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_GetInfo));
  auto result = request<PoolInfo>(client, fbb, 3000000, "getInfo", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<PoolInfoT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<BlockT> ioGetCurrentBlock(p2pNode *client)
{
  uint8_t buffer[4096];
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_GetCurrentBlock));
  auto result = request<Block>(client, fbb, 3000000, "getCurrentBlock", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<BlockT>(result->UnPack());
  } else {
    return 0;
  }  
}

std::unique_ptr<BlockTemplateT> ioGetBlockTemplate(p2pNode *client)
{
  uint8_t buffer[4096];
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_GetBlockTemplate));
  auto result = request<BlockTemplate>(client, fbb, 3000000, "getBlockTemplate", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<BlockTemplateT>(result->UnPack());
  } else {
    return 0;
  }  
}

std::unique_ptr<GetBalanceResultT> ioGetBalance(p2pNode *client)
{
  uint8_t buffer[1024];
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_GetBalance));
  auto result = request<GetBalanceResult>(client, fbb, 3000000, "getBalance", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<GetBalanceResultT>(result->UnPack());
  } else {
    return 0;
  }  
}

std::unique_ptr<GetBlockByHashResultT> ioGetBlockByHash(p2pNode *client,
                      std::vector<std::string> &hashes)
{
  const size_t size = std::max(hashes.size(), (size_t)1)*4096;
  std::unique_ptr<uint8_t[]> buffer(new uint8_t[size]);
  flatbuffers::FlatBufferBuilder fbb;  
  auto reqOffset = CreateGetBlockByHashReq(fbb, fbb.CreateVectorOfStrings(hashes));
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_GetBlockByHash, Data_GetBlockByHashReq, reqOffset.Union()));
  
  auto result = request<GetBlockByHashResult>(client, fbb, 3000000, "getBlockByHash", buffer.get(), size);
  if (result) {
    return std::unique_ptr<GetBlockByHashResultT>(result->UnPack());
  } else {
    return 0;
  }    
}

std::unique_ptr<ProofOfWorkResultT> ioSendProofOfWork(p2pNode *client,
                       int64_t height,
                       int64_t time,
                       const std::string &nonce,
                       int64_t extraNonce,
                       const std::string &data)
{
  uint8_t buffer[1024];
  flatbuffers::FlatBufferBuilder fbb;  
  auto reqOffset = CreateProofOfWorkReq(fbb, height, time, fbb.CreateString(nonce), extraNonce, fbb.CreateString(data));
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_SendProofOfWork, Data_ProofOfWorkReq, reqOffset.Union()));
  auto result = request<ProofOfWorkResult>(client, fbb, 60*1000000, "sendProofOfWork", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<ProofOfWorkResultT>(result->UnPack());
  } else {
    return 0;
  }  
}

std::unique_ptr<SendMoneyResultT> ioSendMoney(p2pNode *client,
                                              const std::string &destination,
                                              int64_t amount)
{
  uint8_t buffer[1024];  
  flatbuffers::FlatBufferBuilder fbb;  
  auto reqOffset = CreateSendMoneyReq(fbb, fbb.CreateString(destination), amount);  
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_SendMoney, Data_SendMoneyReq, reqOffset.Union()));  
  auto result = request<SendMoneyResult>(client, fbb, 60*1000000, "sendMoney", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<SendMoneyResultT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<WalletResultT> ioZGetBalance(p2pNode *client, const std::string &address)
{
  uint8_t buffer[1024];    
  flatbuffers::FlatBufferBuilder fbb;
  auto wrbAddress = fbb.CreateString(address);
  WalletReqBuilder wrb(fbb);
  wrb.add_singleAddress(wrbAddress);
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_ZGetBalance, Data_WalletReq, wrb.Finish().Union()));
  auto result = request<WalletResult>(client, fbb, 3000000, "ZGetBalance", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<WalletResultT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<WalletResultT> ioZSendMoney(p2pNode *client,
                  const std::string &source,
                  const std::vector<ZDestinationT> &destinations)
{
  uint8_t buffer[1024];     
  flatbuffers::FlatBufferBuilder fbb;

  std::vector<flatbuffers::Offset<ZDestination>> offsets;
  for (auto &d: destinations)
    offsets.push_back(CreateZDestination(fbb, &d));
  
  auto sourceOff = fbb.CreateString(source);
  auto destinationsOff = fbb.CreateVector(offsets);
  WalletReqBuilder wrb(fbb);  
  wrb.add_singleAddress(sourceOff);
  wrb.add_destinations(destinationsOff);
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_ZSendMoney, Data_WalletReq, wrb.Finish().Union()));
  auto result = request<WalletResult>(client, fbb, 3000000, "ZSendMoney", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<WalletResultT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<WalletResultT> ioListUnspent(p2pNode *client)
{
  const size_t size = 1048576;
  std::unique_ptr<uint8_t[]> buffer(new uint8_t[size]);  
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_ListUnspent));
  auto result = request<WalletResult>(client, fbb, 3000000, "listUnspent", buffer.get(), size);
  if (result) {
    return std::unique_ptr<WalletResultT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<WalletResultT> ioAsyncOperationStatus(p2pNode *client, const std::string &asyncOpid)
{
  uint8_t buffer[1024];     
  flatbuffers::FlatBufferBuilder fbb;  
  auto asyncOpIdOffset = fbb.CreateString(asyncOpid);
  WalletReqBuilder wrb(fbb);  
  if (!asyncOpid.empty())
    wrb.add_singleAsyncOperationId(asyncOpIdOffset);
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_ZAsyncOperationStatus, Data_WalletReq, wrb.Finish().Union()));
  auto result = request<WalletResult>(client, fbb, 3000000, "asyncOperationStatus", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<WalletResultT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<QueryResultT> ioQueryFoundBlocks(p2pNode *client,
                        int64_t height,
                        const std::string &hash,
                        unsigned count)
{
  const size_t size = count*4096;
  std::unique_ptr<uint8_t[]> buffer(new uint8_t[size]);  
  flatbuffers::FlatBufferBuilder fbb;  
  auto hashOff = fbb.CreateString(hash);
  QueryBuilder qb(fbb);
  qb.add_heightFrom(height);
  qb.add_hashFrom(hashOff);
  qb.add_count(count);
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_QueryFoundBlocks, Data_Query, qb.Finish().Union()));
  auto result = request<QueryResult>(client, fbb, 3000000, "queryFoundBlocks", buffer.get(), size);
  if (result) {
    return std::unique_ptr<QueryResultT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<QueryResultT> ioQueryPoolBalance(p2pNode *client, int64_t time, unsigned count)
{
  const size_t size = count*512;
  std::unique_ptr<uint8_t[]> buffer(new uint8_t[size]); 
  flatbuffers::FlatBufferBuilder fbb;  
  QueryBuilder qb(fbb);
  qb.add_timeFrom(time);
  qb.add_count(count);
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_QueryPoolBalance, Data_Query, qb.Finish().Union()));
  auto result = request<QueryResult>(client, fbb, 3000000, "queryPoolBalance", buffer.get(), size);
  if (result) {
    return std::unique_ptr<QueryResultT>(result->UnPack());
  } else {
    return 0;
  }
}


std::unique_ptr<QueryResultT> ioQueryPayouts(p2pNode *client, 
                    const std::string &userId,
                    GroupByType groupBy,
                    int64_t timeFrom,
                    unsigned count)
{
  const size_t size = count*512;  
  std::unique_ptr<uint8_t[]> buffer(new uint8_t[size]);   
  flatbuffers::FlatBufferBuilder fbb;
  auto userIdOff = fbb.CreateString(userId);
  QueryBuilder qb(fbb);
  qb.add_userId(userIdOff); 
  qb.add_groupBy(groupBy);
  qb.add_timeFrom(timeFrom);
  qb.add_count(count);
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_QueryPayouts, Data_Query, qb.Finish().Union()));
  auto result = request<QueryResult>(client, fbb, 3000000, "queryPayouts", buffer.get(), size);
  if (result) {
    return std::unique_ptr<QueryResultT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<QueryResultT> ioQueryClientStats(p2pNode *client, const std::string &userId)
{
  const size_t size = 1048576;
  std::unique_ptr<uint8_t[]> buffer(new uint8_t[size]);  
  flatbuffers::FlatBufferBuilder fbb;
  auto userIdOff = fbb.CreateString(userId);
  QueryBuilder qb(fbb);
  qb.add_userId(userIdOff);
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_QueryClientStats, Data_Query, qb.Finish().Union()));
  auto result = request<QueryResult>(client, fbb, 3000000, "queryClientStats", buffer.get(), size);
  if (result) {
    return std::unique_ptr<QueryResultT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<QueryResultT> ioQueryPoolStats(p2pNode *client)
{
  uint8_t buffer[4096];   
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_QueryPoolStats, Data_Query, CreateQuery(fbb).Union()));
  auto result = request<QueryResult>(client, fbb, 3000000, "queryPoolStats", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<QueryResultT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<QueryResultT> ioQueryClientInfo(p2pNode *client, const std::string &userId)
{
  uint8_t buffer[4096];     
  flatbuffers::FlatBufferBuilder fbb;
  auto userIdOff = fbb.CreateString(userId);
  QueryBuilder qb(fbb);
  qb.add_userId(userIdOff);
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_QueryClientInfo, Data_Query, qb.Finish().Union()));  
  auto result = request<QueryResult>(client, fbb, 3000000, "queryClientInfo", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<QueryResultT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<QueryResultT> ioUpdateClientInfo(p2pNode *client,
                        const std::string &userId,
                        const std::string &name,
                        const std::string &email,
                        int64_t minimalPayout)
{
  uint8_t buffer[4096];       
  flatbuffers::FlatBufferBuilder fbb;
  auto userIdOff = fbb.CreateString(userId);
  auto ciOff = CreateClientInfo(fbb, 0, 0, 0, fbb.CreateString(name), fbb.CreateString(email), minimalPayout);
  QueryBuilder qb(fbb);
  qb.add_userId(userIdOff);
  qb.add_clientInfo(ciOff);
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_UpdateClientInfo, Data_Query, qb.Finish().Union()));    
  auto result = request<QueryResult>(client, fbb, 3000000, "updateClientInfo", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<QueryResultT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<QueryResultT> ioResendBrokenTx(p2pNode *client, const std::string &userId)
{
  uint8_t buffer[4096];     
  flatbuffers::FlatBufferBuilder fbb;
  auto userIdOff = fbb.CreateString(userId);
  QueryBuilder qb(fbb);
  qb.add_userId(userIdOff);
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_ResendBrokenTx, Data_Query, qb.Finish().Union()));  
  auto result = request<QueryResult>(client, fbb, 3000000, "resendBrokenTx", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<QueryResultT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<QueryResultT> ioMoveBalance(p2pNode *client, const std::string &from, const std::string &to)
{
  uint8_t buffer[4096];     
  flatbuffers::FlatBufferBuilder fbb;
  auto fromff = fbb.CreateString(from);
  auto toOff = fbb.CreateString(to);
  QueryBuilder qb(fbb);
  qb.add_userId(fromff);
  qb.add_targetUserId(toOff);
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_MoveBalance, Data_Query, qb.Finish().Union()));  
  auto result = request<QueryResult>(client, fbb, 3000000, "moveBalance", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<QueryResultT>(result->UnPack());
  } else {
    return 0;
  }
}

std::unique_ptr<QueryResultT> ioManualPayout(p2pNode *client, const std::string &userId)
{
  uint8_t buffer[4096];     
  flatbuffers::FlatBufferBuilder fbb;
  auto userIdOff = fbb.CreateString(userId);
  QueryBuilder qb(fbb);
  qb.add_userId(userIdOff);
  fbb.Finish(CreateP2PMessage(fbb, FunctionId_ManualPayout, Data_Query, qb.Finish().Union()));  
  auto result = request<QueryResult>(client, fbb, 3000000, "manualPayout", buffer, sizeof(buffer));
  if (result) {
    return std::unique_ptr<QueryResultT>(result->UnPack());
  } else {
    return 0;
  }
}
