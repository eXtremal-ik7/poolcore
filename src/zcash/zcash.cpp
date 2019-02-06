#include "poolrpc/poolrpc.h"
#include "poolcommon/pool_generated.h"
#include "chain.h"
#include "key_io.h"
// #include "main.h"
#include "miner.h"
#include "rpcserver.h"
#include "utilmoneystr.h"
#include "wallet/wallet.h"
#include "wallet/asyncrpcoperation_sendmany.h"
#include "asyncrpcqueue.h"

extern CBlockIndex* pindexBestHeader;
extern CWallet* pwalletMain;
extern BlockMap mapBlockIndex;
bool ProcessBlockFound(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey);

#define V3_JS_DESCRIPTION_SIZE    (GetSerializeSize(JSDescription(), SER_NETWORK, (OVERWINTER_TX_VERSION | (1 << 31))))
#define Z_SENDMANY_MAX_ZADDR_OUTPUTS_BEFORE_SAPLING    ((MAX_TX_SIZE_BEFORE_SAPLING / V3_JS_DESCRIPTION_SIZE) - 1)
#define CTXIN_SPEND_DUST_SIZE   148
#define CTXOUT_REGULAR_SIZE     34

CAmount getBalanceTaddr(std::string transparentAddress, int minDepth=1, bool ignoreUnspendable=true);
CAmount getBalanceZaddr(std::string address, int minDepth = 1, bool ignoreUnspendable=true);
std::shared_ptr<AsyncRPCQueue> getAsyncRPCQueue();

uint16_t p2pPort() { return static_cast<uint16_t>(GetArg("-p2pport", 12200)); }

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
  CTxDestination dest = DecodeDestination(destination);
  CScript scriptPubKey = GetScriptForDestination(dest);
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
    out.hashreserved = "";
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
    out.hashreserved = "";
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
  bool fromTaddr = false;
  CTxDestination taddr = DecodeDestination(addr);
  fromTaddr = IsValidDestination(taddr);
  if (!fromTaddr) {
    auto res = DecodePaymentAddress(addr);
    if (!IsValidPaymentAddress(res)) {
      error = "Invalid from address, should be a taddr or zaddr.";
      return -1;
    }
    // TODO: Add Sapling support. For now, ensure we can freely convert.
    assert(boost::get<libzcash::SproutPaymentAddress>(&res) != nullptr);
    auto zaddr = boost::get<libzcash::SproutPaymentAddress>(res);
    if (!(pwalletMain->HaveSpendingKey(zaddr) || pwalletMain->HaveViewingKey(zaddr))) {
      error = "From address does not belong to this node, zaddr spending key or viewing key not found.";
      return -1;
    }
  }

  CAmount nBalance = 0;
  if (fromTaddr) {
    nBalance = getBalanceTaddr(addr, nMinDepth, false);
  } else {
    nBalance = getBalanceZaddr(addr, nMinDepth, false);
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
  CTxDestination taddr = DecodeDestination(fromaddress);
  fromTaddr = IsValidDestination(taddr);
  libzcash::SproutPaymentAddress zaddr;
  if (!fromTaddr) {
    auto res = DecodePaymentAddress(fromaddress);
    if (!IsValidPaymentAddress(res)) {
      // invalid
      error = "Invalid from address, should be a taddr or zaddr.";
      return "";
    }
    // TODO: Add Sapling support. For now, ensure we can freely convert.
    assert(boost::get<libzcash::SproutPaymentAddress>(&res) != nullptr);
    zaddr = boost::get<libzcash::SproutPaymentAddress>(res);
  }

  // Check that we have the spending key
  if (!fromTaddr) {
    if (!pwalletMain->HaveSpendingKey(zaddr)) {
       error = "From address does not belong to this node, zaddr spending key not found.";
       return "";
    }
  }

  if (destinations.size()==0) {
    error = "Invalid parameter, amounts array is empty.";
    return "";
  }

  // Keep track of addresses to spot duplicates
  set<std::string> setAddress;

  // Recipients
  std::vector<SendManyRecipient> taddrRecipients;
  std::vector<SendManyRecipient> zaddrRecipients;
  CAmount nTotalOut = 0;

  for (auto o: destinations) {
    std::string address = o.address;
    bool isZaddr = false;
    CTxDestination taddr = DecodeDestination(address);
    if (!IsValidDestination(taddr)) {
      if (IsValidPaymentAddressString(address)) {
        isZaddr = true;
      } else {
        error = "Invalid parameter, unknown address format";
        return "";
      }
    }

    if (setAddress.count(address)) {
      error = "Invalid parameter, duplicated address";
      return "";
    }
    
    setAddress.insert(address);
    
    std::string memo = o.memo;
    if (!isZaddr) {
      error = "Memo cannot be used with a taddr.  It can only be used with a zaddr.";
      return "";
    } else if (!IsHex(memo)) {
      error = "Invalid parameter, expected memo data in hexadecimal format.";
      return "";
    }
    
    if (memo.length() > ZC_MEMO_SIZE*2) {
      error = "Invalid parameter, size of memo is larger than maximum allowed";
      return "";
    }

    CAmount nAmount = o.amount;
    if (nAmount < 0) {
      error = "Invalid parameter, amount must be positive";
      return "";
    }

    if (isZaddr) {
      zaddrRecipients.push_back( SendManyRecipient(address, nAmount, memo) );
    } else {
      taddrRecipients.push_back( SendManyRecipient(address, nAmount, memo) );
    }

    nTotalOut += nAmount;
  }

  int nextBlockHeight = chainActive.Height() + 1;
  CMutableTransaction mtx;
  mtx.fOverwintered = true;
  mtx.nVersionGroupId = SAPLING_VERSION_GROUP_ID;
  mtx.nVersion = SAPLING_TX_VERSION;
  unsigned int max_tx_size = MAX_TX_SIZE_AFTER_SAPLING;
  if (!NetworkUpgradeActive(nextBlockHeight, Params().GetConsensus(), Consensus::UPGRADE_SAPLING)) {
    if (NetworkUpgradeActive(nextBlockHeight, Params().GetConsensus(), Consensus::UPGRADE_OVERWINTER)) {
      mtx.nVersionGroupId = OVERWINTER_VERSION_GROUP_ID;
      mtx.nVersion = OVERWINTER_TX_VERSION;
    } else {
      mtx.fOverwintered = false;
      mtx.nVersion = 2;
    }

    max_tx_size = MAX_TX_SIZE_BEFORE_SAPLING;

    // Check the number of zaddr outputs does not exceed the limit.
    if (zaddrRecipients.size() > Z_SENDMANY_MAX_ZADDR_OUTPUTS_BEFORE_SAPLING)  {
      error = "Invalid parameter, too many zaddr outputs";
      return "";
    }
  }

  // As a sanity check, estimate and verify that the size of the transaction will be valid.
  // Depending on the input notes, the actual tx size may turn out to be larger and perhaps invalid.
  size_t txsize = 0;
  for (int i = 0; i < zaddrRecipients.size(); i++) {
    // TODO Check whether the recipient is a Sprout or Sapling address
    JSDescription jsdesc;

    if (mtx.fOverwintered && (mtx.nVersion >= SAPLING_TX_VERSION)) {
      jsdesc.proof = GrothProof();
    }

    mtx.vjoinsplit.push_back(jsdesc);
  }
  
  CTransaction tx(mtx);
  txsize += GetSerializeSize(tx, SER_NETWORK, tx.nVersion);
  if (fromTaddr) {
    txsize += CTXIN_SPEND_DUST_SIZE;
    txsize += CTXOUT_REGULAR_SIZE;      // There will probably be taddr change
  }
  
  txsize += CTXOUT_REGULAR_SIZE * taddrRecipients.size();
  if (txsize > max_tx_size) {
    error = "Too many outputs, size of raw transaction would be larger than limit";
    return "";
  }

  // Minimum confirmations
  int nMinDepth = 1;

  // Fee in Zatoshis, not currency format)
  CAmount nFee = ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE;

  // Contextual transaction we will build on
  CMutableTransaction contextualTx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), nextBlockHeight);
  bool isShielded = !fromTaddr || zaddrRecipients.size() > 0;
  if (contextualTx.nVersion == 1 && isShielded) {
    contextualTx.nVersion = 2; // Tx format should support vjoinsplits 
  }

  // Create operation and add to global queue
  std::shared_ptr<AsyncRPCQueue> q = getAsyncRPCQueue();
  std::shared_ptr<AsyncRPCOperation> operation( new AsyncRPCOperation_sendmany(contextualTx, fromaddress, taddrRecipients, zaddrRecipients, nMinDepth, nFee, UniValue()) );
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

  bool fIncludeWatchonly = false;
  std::set<libzcash::PaymentAddress> zaddrs = {};  
  LOCK2(cs_main, pwalletMain->cs_wallet);

  // User did not provide zaddrs, so use default i.e. all addresses
  // TODO: Add Sapling support
  std::set<libzcash::SproutPaymentAddress> sproutzaddrs = {};
  pwalletMain->GetPaymentAddresses(sproutzaddrs);
  zaddrs.insert(sproutzaddrs.begin(), sproutzaddrs.end());

  if (zaddrs.size() > 0) {
    std::vector<CUnspentSproutNotePlaintextEntry> entries;
    pwalletMain->GetUnspentFilteredNotes(entries, zaddrs, nMinDepth, nMaxDepth, !fIncludeWatchonly);
    for (CUnspentSproutNotePlaintextEntry & entry : entries) {
      ListUnspentElementT element;
      element.address = EncodePaymentAddress(entry.address);
      element.amount = entry.plaintext.value();
      element.confirmations = entry.nHeight;
      element.spendable = pwalletMain->HaveSpendingKey(boost::get<libzcash::SproutPaymentAddress>(entry.address));
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
    UniValue err = operation->getError();
    if (!err.isNull())
      status.error = find_value(err.get_obj(), "message").get_str();
    UniValue result = operation->getResult();
    if (!result.isNull())
      status.txid = find_value(result.get_obj(), "txid").get_str();
    
    out.push_back(status);  
  }
}
