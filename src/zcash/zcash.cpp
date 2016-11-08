#include "poolrpc/poolrpc.h"
#include "poolcommon/pool_generated.h"
#include "main.h"
#include "miner.h"
#include "utilmoneystr.h"
#include "wallet/wallet.h"
#include "wallet/asyncrpcoperation_sendmany.h"
#include "asyncrpcqueue.h"

extern CBlockIndex* pindexBestHeader;
extern CWallet* pwalletMain;
extern BlockMap mapBlockIndex;
bool ProcessBlockFound(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey);

CAmount getBalanceTaddr(std::string transparentAddress, int minDepth=1);
CAmount getBalanceZaddr(std::string address, int minDepth = 1);
std::shared_ptr<AsyncRPCQueue> getAsyncRPCQueue();

int p2pPort() { return GetArg("-p2pport", 12200); }

const char *getCoinName() { return "ZEC"; }
WalletTy *getMainWallet() { return (WalletTy*)pwalletMain; }

BlockIndexTy *getCurrentBlock()
{
  return (BlockIndexTy*)pindexBestHeader;
}

BlockIndexTy *getBlockByHash(const std::string &hash)
{
  uint256 hash256;
  hash256.SetHex(hash);
  return (BlockIndexTy*)mapBlockIndex[hash256];
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
    if(!pindexBestHeader)
      MilliSleep(100);
    else
      break;
  }

  return (BlockTemplateTy*)CreateNewBlockWithKey(*(CReserveKey*)reserveKey);
}

ReserveKeyTy *createReserveKey(WalletTy *wallet)
{
  return (ReserveKeyTy*)(new CReserveKey((CWallet*)wallet));
}

void mutateBlockTemplate(BlockTemplateTy *block, unsigned *extraNonce)
{
  CBlockTemplate *blockTemplate = (CBlockTemplate*)block;
  IncrementExtraNonce(&blockTemplate->block, pindexBestHeader, *extraNonce);
}

bool checkWork(BlockTemplateTy *blockTemplate, ReserveKeyTy *key, ProofOfWorkReqT &proofOfWork, int64_t *generatedCoins)
{
  CBlockTemplate *tmpl = (CBlockTemplate*)blockTemplate;
  CBlock *block = &tmpl->block;
  unsigned N = proofOfWork.extraNonce-1;
  IncrementExtraNonce(block, pindexBestHeader, N);
  block->nTime = proofOfWork.time;
  block->nNonce.SetHex(proofOfWork.nonce.c_str());
  block->nSolution.assign(proofOfWork.data.begin(), proofOfWork.data.end());
  bool result = ProcessBlockFound(block, *pwalletMain, *(CReserveKey*)key);
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
  CWalletTx wtx;
  wtx.mapValue["comment"] = "payout";

  CReserveKey reservekey(pwalletMain);
  
  CBitcoinAddress address(destination);  
  CScript scriptPubKey = GetScriptForDestination(address.Get());
  CRecipient recipient = {scriptPubKey, amount, false};
  std::vector<CRecipient> recipients = {recipient};  
  
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
  
  CAmount feeRequired;
  int changePosSet;
  if (!pwalletMain->CreateTransaction(recipients, wtx, reservekey, feeRequired, changePosSet, result.error)) {
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
    out.hashreserved = blockIndex->hashReserved.GetHex();
    out.time = blockIndex->nTime;
    
    if (blockIndex == pindexBestHeader || pindexBestHeader->GetAncestor(blockIndex->nHeight)) {      
      CBlock block;
      ReadBlockFromDisk(block, blockIndex);
      CMerkleTx txGen(block.vtx[0]);
      txGen.SetMerkleBranch(block);
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
    const CChainParams &chainparams = Params();    
    CBlock *block = &((CBlockTemplate*)tmpl)->block;
    out.bits = block->nBits;
    out.prevhash = block->hashPrevBlock.ToString();
    out.hashreserved = block->hashReserved.ToString();
    out.merkle = block->hashMerkleRoot.ToString();
    out.time = block->nTime;
    out.extraNonce = extraNonce;
    out.equilHashK = chainparams.EquihashK();
    out.equilHashN = chainparams.EquihashN();
  } else {
    out.bits = 0;
    out.prevhash = "";
    out.merkle = "";
    out.time = 0;
    out.extraNonce = 0;
  }
}

int64_t ZGetbalance(const std::string &addr, std::string &error)
{
  LOCK2(cs_main, pwalletMain->cs_wallet);
  int nMinDepth = 1;

  // Check that the from address is valid.
  CBitcoinAddress taddr(addr);
  bool fromTaddr = taddr.IsValid();
  libzcash::PaymentAddress zaddr;
  if (!fromTaddr) {
    CZCPaymentAddress address(addr);
    try {
      zaddr = address.Get();
    } catch (std::runtime_error) {
      error = "getZBalance: Invalid from address, should be a taddr or zaddr.";
      return -1;
    }
    if (!pwalletMain->HaveSpendingKey(zaddr)) {
      error = "getZBalance: From address does not belong to this node, zaddr spending key not found.";
      return -1;
    }
  }

  CAmount nBalance = 0;
  if (fromTaddr) {
    nBalance = getBalanceTaddr(addr, nMinDepth);
  } else {
    nBalance = getBalanceZaddr(addr, nMinDepth);  
  }

  return nBalance;
}

std::string ZSendMoney(const std::string &source, const std::vector<ZDestinationT> &destinations, std::string &error)
{
  for (auto &d: destinations)
    fprintf(stderr, "to %s, %lu, %s\n", d.address.c_str(), (unsigned long)d.amount, d.memo.c_str());
  
  LOCK2(cs_main, pwalletMain->cs_wallet);

  // Check that the from address is valid.
  auto fromaddress = source;
  bool fromTaddr = false;
  CBitcoinAddress taddr(fromaddress);
  fromTaddr = taddr.IsValid();
  libzcash::PaymentAddress zaddr;
  if (!fromTaddr) {
    CZCPaymentAddress address(fromaddress);
    try {
      zaddr = address.Get();
    } catch (std::runtime_error) {
      // invalid
      error = "Invalid from address, should be a taddr or zaddr.";
      return "";
    }
  }

  // Check that we have the spending key
  if (!fromTaddr) {
    if (!pwalletMain->HaveSpendingKey(zaddr)) {
      error = "From address does not belong to this node, zaddr spending key not found.";
      return "";
    }
  }

  // Keep track of addresses to spot duplicates
  std::set<std::string> setAddress;

  // Recipients
  std::vector<SendManyRecipient> taddrRecipients;
  std::vector<SendManyRecipient> zaddrRecipients;

  for (auto &d: destinations) {
     bool isZaddr = false;
     CBitcoinAddress taddr(d.address);
     if (!taddr.IsValid()) {
       try {
         CZCPaymentAddress zaddr(d.address);
         zaddr.Get();
         isZaddr = true;
       } catch (std::runtime_error) {
         error = "Invalid parameter, unknown address format";
         return "";
       }
     }

      if (setAddress.count(d.address)) {
        error = "Invalid parameter, duplicated address";
        return "";
      }
      
      setAddress.insert(d.address);

      if (!d.memo.empty()) {
        if (!isZaddr) {
          error = "Memo can not be used with a taddr.  It can only be used with a zaddr.";
          return "";
        } else if (!IsHex(d.memo)) {
          error = "Invalid parameter, expected memo data in hexadecimal format.";   
          return "";
        }
        if (d.memo.length() > ZC_MEMO_SIZE*2) {
          error = "Invalid parameter, size of memo is larger than maximum allowed";
          return "";
        }
      }

      if (d.amount < 0) {
        error = "Invalid parameter, amount must be positive";
        return "";
      }

      if (isZaddr) {
        zaddrRecipients.push_back( SendManyRecipient(d.address, d.amount, d.memo) );
      } else {
        taddrRecipients.push_back( SendManyRecipient(d.address, d.amount, d.memo) );
      }
    }

  // Minimum confirmations
  int nMinDepth = 1;

  // Create operation and add to global queue
  std::shared_ptr<AsyncRPCQueue> q = getAsyncRPCQueue();
  std::shared_ptr<AsyncRPCOperation> operation( new AsyncRPCOperation_sendmany(fromaddress, taddrRecipients, zaddrRecipients, nMinDepth) );
  q->addOperation(operation);
  AsyncRPCOperationId operationId = operation->getId();
  return operationId.c_str();
}

void listUnspent(std::vector<ListUnspentElementT> &result, std::string &error)
{
  result.clear();
  if (!pwalletMain)
    return;

  int nMinDepth = 1;
  int nMaxDepth = 9999999;  

  std::vector<COutput> vecOutputs;
  LOCK2(cs_main, pwalletMain->cs_wallet);
  pwalletMain->AvailableCoins(vecOutputs, false, NULL, true);
  BOOST_FOREACH(const COutput& out, vecOutputs) {
    if (out.nDepth < nMinDepth || out.nDepth > nMaxDepth)
      continue;
  
    CAmount nValue = out.tx->vout[out.i].nValue;
    
    CTxDestination address;
    if (ExtractDestination(out.tx->vout[out.i].scriptPubKey, address)) {
      ListUnspentElementT element;
      element.address = CBitcoinAddress(address).ToString();
      element.amount = nValue;
      element.confirmations = out.nDepth;
      element.spendable = out.fSpendable;
      result.push_back(element);
    }
  }
}

void ZAsyncOperationStatus(const std::string &id, std::vector<AsyncOperationStatusT> &out, std::string &error)
{
  out.clear();
  LOCK2(cs_main, pwalletMain->cs_wallet);

  std::set<AsyncRPCOperationId> filter;
  if (!id.empty())
    filter.insert(id);
  bool useFilter = !id.empty();

  std::shared_ptr<AsyncRPCQueue> q = getAsyncRPCQueue();
  std::vector<AsyncRPCOperationId> ids = q->getAllOperationIds();

  for (auto id : ids) {
    if (useFilter && !filter.count(id))
      continue;
 
    std::shared_ptr<AsyncRPCOperation> operation = q->getOperationForId(id);
    if (!operation) {
      continue;
    }
        
    AsyncOperationStatusT status;
    status.id = operation->getId();
    
    switch (operation->getState()) {
      case OperationStatus::READY :
        status.state = AsyncOpState_READY;
        break;
      case OperationStatus::EXECUTING :
        status.state = AsyncOpState_EXECUTING;
        break;
      case OperationStatus::CANCELLED :
        status.state = AsyncOpState_CANCELLED;
        break;
      case OperationStatus::FAILED :
        status.state = AsyncOpState_FAILED;
        break;
      case OperationStatus::SUCCESS :
        status.state = AsyncOpState_SUCCESS;
        break;
    }

    status.creationTime = operation->getCreationTime();
    Value err = operation->getError();
    if (!err.is_null())
      status.error = find_value(err.get_obj(), "message").get_str();
    Value result = operation->getResult();
    if (!result.is_null())
      status.txid = find_value(result.get_obj(), "txid").get_str();
    
    out.push_back(status);  
  }
}
