#ifndef __POOL_API_H_
#define __POOL_API_H_

#include "poolcommon/pool_generated.h"

class p2pNode;

std::unique_ptr<PoolInfoT> ioGetInfo(p2pNode *client);

std::unique_ptr<BlockT> ioGetCurrentBlock(p2pNode *client);

std::unique_ptr<BlockTemplateT> ioGetBlockTemplate(p2pNode *client);

std::unique_ptr<GetBalanceResultT> ioGetBalance(p2pNode *client);

std::unique_ptr<GetBlockByHashResultT> ioGetBlockByHash(p2pNode *client,
                      std::vector<std::string> &hashes);

std::unique_ptr<ProofOfWorkResultT> ioSendProofOfWork(p2pNode *client,
                       int64_t height,
                       int64_t time,
                       const std::string &nonce,
                       int64_t extraNonce,
                       const std::string &data);

std::unique_ptr<SendMoneyResultT> ioSendMoney(p2pNode *client,
                                              const std::string &destination,
                                              int64_t amount);

std::unique_ptr<WalletResultT> ioZGetBalance(p2pNode *client, const std::string &address);

std::unique_ptr<WalletResultT> ioZSendMoney(p2pNode *client,
                  const std::string &source,
                  const std::vector<ZDestinationT> &destinations);

std::unique_ptr<WalletResultT> ioListUnspent(p2pNode *client);

std::unique_ptr<WalletResultT> ioAsyncOperationStatus(p2pNode *client, const std::string &asyncOpid);

// Backend API

std::unique_ptr<QueryResultT> ioQueryFoundBlocks(p2pNode *client,
                        int64_t height,
                        const std::string &hash,
                        unsigned count);

std::unique_ptr<QueryResultT> ioQueryPoolBalance(p2pNode *client, int64_t time, unsigned count);

std::unique_ptr<QueryResultT> ioQueryPayouts(p2pNode *client, 
                    const std::string &userId,
                    GroupByType groupBy,
                    int64_t timeFrom,
                    unsigned count);

std::unique_ptr<QueryResultT> ioQueryClientStats(p2pNode *client, const std::string &userId);

std::unique_ptr<QueryResultT> ioQueryPoolStats(p2pNode *client);

std::unique_ptr<QueryResultT> ioQueryClientInfo(p2pNode *client, const std::string &userId);

std::unique_ptr<QueryResultT> ioUpdateClientInfo(p2pNode *client,
                        const std::string &userId,
                        const std::string &name,
                        const std::string &email,
                        int64_t minimalPayout);

std::unique_ptr<QueryResultT> ioResendBrokenTx(p2pNode *client, const std::string &userId);

std::unique_ptr<QueryResultT> ioMoveBalance(p2pNode *client, const std::string &from, const std::string &to);

std::unique_ptr<QueryResultT> ioManualPayout(p2pNode *client, const std::string &userId);


#endif //__POOL_API_H_
