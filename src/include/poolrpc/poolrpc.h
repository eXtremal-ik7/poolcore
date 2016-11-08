#ifndef __POOLRPC_H_
#define __POOLRPC_H_

#include "poolcommon/pool_generated.h"

struct BlockTemplateTy;
struct BlockIndexTy;
struct BlockTy;
struct ReserveKeyTy;
struct WalletTy;

int p2pPort();
const char *getCoinName();
WalletTy *getMainWallet();
BlockIndexTy *getCurrentBlock();
BlockIndexTy *getBlockByHash(const std::string &hash);
void deleteBlockTemplate(BlockTemplateTy *block);

ReserveKeyTy *createReserveKey(WalletTy *wallet);
BlockTemplateTy *createBlockTemplate(ReserveKeyTy *reserveKey);
void mutateBlockTemplate(BlockTemplateTy *block, unsigned *extraNonce);
bool checkWork(BlockTemplateTy *blockTemplate, ReserveKeyTy *key, ProofOfWorkReqT &proofOfWork, int64_t *generatedCoins);
void getBalance(GetBalanceResultT *balance);
void sendMoney(const char *destination, int64_t amount, SendMoneyResultT &result);

void createBlockRecord(BlockIndexTy *block, BlockT &out);
void createBlockTemplateRecord(BlockTemplateTy *block, unsigned extraNonce, BlockTemplateT &out);

// ZCash specific
int64_t ZGetbalance(const std::string &zaddr, std::string &error);
std::string ZSendMoney(const std::string &source, const std::vector<ZDestinationT> &destinations, std::string &error);
void listUnspent(std::vector<ListUnspentElementT> &out, std::string &error);
void ZAsyncOperationStatus(const std::string &id, std::vector<AsyncOperationStatusT> &out, std::string &error);

#endif //__POOLRPC_H_
