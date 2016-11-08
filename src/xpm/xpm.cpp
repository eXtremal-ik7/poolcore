#include "p2p/p2p.h"
#include "poolrpc/poolrpc.h"
// #include "poolcommon/pool_generated.h"
#include "main.h"
#include "wallet.h"

extern CBlockIndex* pindexBest;
extern CWallet* pwalletMain;
extern std::map<uint256, CBlockIndex*> mapBlockIndex;

int p2pPort() { return GetArg("-p2pport", 12200); }

const char *getCoinName() { return "XPM"; }
WalletTy *getMainWallet() { return (WalletTy*)pwalletMain; }

BlockIndexTy *getCurrentBlock()
{
  return (BlockIndexTy*)pindexBest;
}

BlockIndexTy *getBlockByHash(const std::string &hash)
{
  return (BlockIndexTy*)mapBlockIndex[uint256(hash)];
}

void deleteBlockTemplate(BlockTemplateTy *blockTemplate)
{
  delete (CBlockTemplate*)blockTemplate;
}

BlockTemplateTy *createBlockTemplate(ReserveKeyTy *reserveKey)
{
  while(true) {
    while (vNodes.empty())
      MilliSleep(100);
    if(!pindexBest)
      MilliSleep(100);
    else
      break;
  }

  return (BlockTemplateTy*)CreateNewBlock(*(CReserveKey*)reserveKey);
}

ReserveKeyTy *createReserveKey(WalletTy *wallet)
{
  return (ReserveKeyTy*)(new CReserveKey((CWallet*)wallet));
}

void mutateBlockTemplate(BlockTemplateTy *block, unsigned *extraNonce)
{
  CBlockTemplate *blockTemplate = (CBlockTemplate*)block;
  IncrementExtraNonce(&blockTemplate->block, pindexBest, *extraNonce);
}

bool checkWork(BlockTemplateTy *blockTemplate, ReserveKeyTy *key, ProofOfWorkReqT &proofOfWork, int64_t *generatedCoins)
{
  CBlockTemplate *tmpl = (CBlockTemplate*)blockTemplate;
  CBlock *block = &tmpl->block;
  unsigned N = proofOfWork.extraNonce-1;
  IncrementExtraNonce(block, pindexBest, N);
  block->nTime = proofOfWork.time;
  block->nNonce = xatoi<decltype(block->nNonce)>(proofOfWork.nonce.c_str());
  block->bnPrimeChainMultiplier.SetHex(proofOfWork.data.c_str());
  
  bool result = CheckWork(block, *pwalletMain, *(CReserveKey*)key);
  if (result) {
    *generatedCoins = block->vtx[0].vout[0].nValue;
  } else {
    *generatedCoins = 0;
  }

  return result; 
}

void getBalance(GetBalanceResultT *balance)
{
  balance->balance = pwalletMain->GetBalance();
  balance->immature = pwalletMain->GetImmatureBalance();
}

void sendMoney(const char *destination, int64_t amount, SendMoneyResultT &result)
{
  CBitcoinAddress address(destination);
  CWalletTx wtx;
  CScript scriptPubKey;
  CReserveKey reservekey(pwalletMain);
  scriptPubKey.SetDestination(address.Get());
  wtx.mapValue["comment"] = "payout";
  
  if (amount <= 0) {
    result.success = false;
    result.error = "Invalid amount";
    return;
  }
  
  if (pwalletMain->IsLocked()) {
    result.success = false;
    result.error = "Error: Wallet locked, unable to create transaction!";
    return;
  }
  
  int64 feeRequired;
  if (!pwalletMain->CreateTransaction(scriptPubKey, amount, wtx, reservekey, feeRequired, result.error)) {
    result.success = false;
    if (amount + feeRequired > pwalletMain->GetBalance())
      result.error = strprintf(_("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds!"), FormatMoney(feeRequired).c_str());
    return;
  }
  
  if (!pwalletMain->CommitTransaction(wtx, reservekey)) {
    result.success = false;
    result.error = "Error: The transaction was rejected! This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.";
    return;
  }

  result.success = true;
  result.error.clear();
  result.remaining = pwalletMain->GetBalance();
  result.fee = feeRequired;
  result.txid = wtx.GetHash().ToString();
}

void createBlockRecord(BlockIndexTy *idx, BlockT &out)
{
  if (idx) {
    CBlockIndex *blockIndex = (CBlockIndex*)idx;
    out.height = blockIndex->nHeight;
    out.bits = blockIndex->nBits;
    out.hash = blockIndex->phashBlock->GetHex();
    out.prevhash = blockIndex->pprev->phashBlock->GetHex();
    out.merkle = blockIndex->hashMerkleRoot.ToString();
    out.time = blockIndex->nTime;
    
    if (blockIndex->IsInMainChain()) {
      CBlock block;
      block.ReadFromDisk(blockIndex);
      CMerkleTx txGen(block.vtx[0]);
      txGen.SetMerkleBranch(&block);
      out.confirmations = txGen.GetDepthInMainChain();        
    } else {
      out.confirmations = -1;
    }
  } else {
    out.height = 0;
    out.bits = 0;
    out.hash = "";
    out.prevhash = "";
    out.merkle = "";
    out.time = 0;
    out.confirmations = -1;
  }
}

void createBlockTemplateRecord(BlockTemplateTy *tmpl, unsigned extraNonce, BlockTemplateT &out)
{
  if (tmpl) {
    CBlock *block = &((CBlockTemplate*)tmpl)->block;
    out.bits = block->nBits;
    out.prevhash = block->hashPrevBlock.ToString();
    out.merkle = block->hashMerkleRoot.ToString();
    out.time = block->nTime;
    out.extraNonce = extraNonce;
  } else {
    out.bits = 0;
    out.prevhash = "";
    out.merkle = "";
    out.time = 0;
    out.extraNonce = 0;
  }
}

void listUnspent(std::vector<ListUnspentElementT> &out, std::string &error)
{
  error = "XPM not supported zcash features";  
}

int64_t ZGetbalance(const std::string &zaddr, std::string &error)
{
  error = "XPM not supported zcash features";
  return -1;
}

std::string ZSendMoney(const std::string &souce, const std::vector<ZDestinationT> &destinations, std::string &error)
{
  error = "XPM not supported zcash features";  
  return "";
}

void ZAsyncOperationStatus(const std::string &id, std::vector<AsyncOperationStatusT> &out, std::string &error)
{
  error = "XPM not supported zcash features";  
}
