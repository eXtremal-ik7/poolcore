#include "poolrpc/poolrpc.h"
#include "poolcommon/pool_generated.h"

#undef NDEBUG
#include "chain.h"
#include "key_io.h"
#include "miner.h"
#include "rpc/server.h"
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

template <unsigned int BITS>
void base_blob<BITS>::SetHex(const char* psz)
{
    memset(data, 0, sizeof(data));

    // skip leading spaces
    while (isspace(*psz))
        psz++;

    // skip 0x
    if (psz[0] == '0' && tolower(psz[1]) == 'x')
        psz += 2;

    // hex string to uint
    const char* pbegin = psz;
    while (::HexDigit(*psz) != -1)
        psz++;
    psz--;
    uint8_t* p1 = data;
    uint8_t* pend = p1 + WIDTH;
    while (psz >= pbegin && p1 < pend) {
        *p1 = static_cast<uint8_t>(::HexDigit(*psz--));
        if (psz >= pbegin) {
            *p1 |= (static_cast<uint8_t>(::HexDigit(*psz--)) << 4);
            p1++;
        }
    }
}

template <unsigned int BITS>
void base_blob<BITS>::SetHex(const std::string& str)
{
    SetHex(str.c_str());
}

uint16_t p2pPort() { return static_cast<uint16_t>(GetArg("-p2pport", 12200)); }

const char *getCoinName() { return "ZEC"; }
WalletTy *getMainWallet() { return reinterpret_cast<WalletTy*>(pwalletMain); }

BlockIndexTy *getCurrentBlock()
{
  return reinterpret_cast<BlockIndexTy*>(pindexBestHeader);
}

BlockIndexTy *getBlockByHash(const std::string &hash)
{
  uint256 hash256;
  hash256.SetHex(hash);
  return reinterpret_cast<BlockIndexTy*>(mapBlockIndex[hash256]);
}

void deleteBlockTemplate(BlockTemplateTy *blockTemplate)
{
  delete reinterpret_cast<CBlockTemplate*>(blockTemplate);
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

  return reinterpret_cast<BlockTemplateTy*>(CreateNewBlockWithKey(*reinterpret_cast<CReserveKey*>(reserveKey)));
}

ReserveKeyTy *createReserveKey(WalletTy *wallet)
{
  return reinterpret_cast<ReserveKeyTy*>(new CReserveKey(reinterpret_cast<CWallet*>(wallet)));
}

void mutateBlockTemplate(BlockTemplateTy *block, unsigned *extraNonce)
{
  CBlockTemplate *blockTemplate = reinterpret_cast<CBlockTemplate*>(block);
  IncrementExtraNonce(&blockTemplate->block, pindexBestHeader, *extraNonce);
}

bool checkWork(BlockTemplateTy *blockTemplate, ReserveKeyTy *key, ProofOfWorkReqT &proofOfWork, int64_t *generatedCoins)
{
  CBlockTemplate *tmpl = reinterpret_cast<CBlockTemplate*>(blockTemplate);
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
  CBlockIndex *blockIndex = (CBlockIndex*)idx;
  if (blockIndex && blockIndex->pprev) {
    out.height = blockIndex->nHeight;
    out.bits = blockIndex->nBits;
    out.hash = blockIndex->phashBlock->GetHex();
    out.prevhash = blockIndex->pprev->phashBlock->GetHex();
    out.merkle = blockIndex->hashMerkleRoot.ToString();
    out.hashreserved = blockIndex->hashFinalSaplingRoot.ToString();
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
    out.hashreserved = block->hashFinalSaplingRoot.ToString();
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
      if (!boost::apply_visitor(PaymentAddressBelongsToWallet(pwalletMain), res)) {
        error = "From address does not belong to this node, spending key or viewing key not found.";
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
  bool fromSapling = false;
  CTxDestination taddr = DecodeDestination(fromaddress);
  fromTaddr = IsValidDestination(taddr);
  if (!fromTaddr) {
      auto res = DecodePaymentAddress(fromaddress);
      if (!IsValidPaymentAddress(res)) {
          // invalid
          error = "Invalid from address, should be a taddr or zaddr.";
          return "";
      }

      // Check that we have the spending key
      if (!boost::apply_visitor(HaveSpendingKeyForPaymentAddress(pwalletMain), res)) {
        error = "From address does not belong to this node, zaddr spending key not found.";
        return "";
      }

      // Remember whether this is a Sprout or Sapling address
      fromSapling = boost::get<libzcash::SaplingPaymentAddress>(&res) != nullptr;
  }
  // This logic will need to be updated if we add a new shielded pool
  bool fromSprout = !(fromTaddr || fromSapling);

  if (destinations.size()==0) {
    error = "Invalid parameter, amounts array is empty.";
    return "";
  }

  // Keep track of addresses to spot duplicates
  set<std::string> setAddress;

  // Track whether we see any Sprout addresses
  bool noSproutAddrs = !fromSprout;

  // Recipients
  std::vector<SendManyRecipient> taddrRecipients;
  std::vector<SendManyRecipient> zaddrRecipients;
  CAmount nTotalOut = 0;

  bool containsSproutOutput = false;
  bool containsSaplingOutput = false;

  for (auto o: destinations) {
      string address = o.address;
      bool isZaddr = false;
      CTxDestination taddr = DecodeDestination(address);
      if (!IsValidDestination(taddr)) {
          auto res = DecodePaymentAddress(address);
          if (IsValidPaymentAddress(res)) {
              isZaddr = true;

              bool toSapling = boost::get<libzcash::SaplingPaymentAddress>(&res) != nullptr;
              bool toSprout = !toSapling;
              noSproutAddrs = noSproutAddrs && toSapling;

              containsSproutOutput |= toSprout;
              containsSaplingOutput |= toSapling;

              // Sending to both Sprout and Sapling is currently unsupported using z_sendmany
              if (containsSproutOutput && containsSaplingOutput) {
                error = "Cannot send to both Sprout and Sapling addresses using z_sendmany";
                return "";
              }

              // If sending between shielded addresses, they must be the same type
              if ((fromSprout && toSapling) || (fromSapling && toSprout)) {
                error = "Cannot send between Sprout and Sapling addresses using z_sendmany";
                return "";
              }
          } else {
            error = std::string("Invalid parameter, unknown address format: ")+address;
            return "";
          }
      }

      if (setAddress.count(address)) {
          error = std::string("Invalid parameter, duplicated address: ")+address;
          return "";
      }
      setAddress.insert(address);

      string memo = o.memo;
      if (!memo.empty()) {
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
          throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, too many zaddr outputs");
      }
  }

  // If Sapling is not active, do not allow sending from or sending to Sapling addresses.
  if (!NetworkUpgradeActive(nextBlockHeight, Params().GetConsensus(), Consensus::UPGRADE_SAPLING)) {
      if (fromSapling || containsSaplingOutput) {
          throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, Sapling has not activated");
      }
  }

  // As a sanity check, estimate and verify that the size of the transaction will be valid.
  // Depending on the input notes, the actual tx size may turn out to be larger and perhaps invalid.
  size_t txsize = 0;
  for (int i = 0; i < zaddrRecipients.size(); i++) {
      auto address = std::get<0>(zaddrRecipients[i]);
      auto res = DecodePaymentAddress(address);
      bool toSapling = boost::get<libzcash::SaplingPaymentAddress>(&res) != nullptr;
      if (toSapling) {
          mtx.vShieldedOutput.push_back(OutputDescription());
      } else {
          JSDescription jsdesc;
          if (mtx.fOverwintered && (mtx.nVersion >= SAPLING_TX_VERSION)) {
              jsdesc.proof = GrothProof();
          }
          mtx.vjoinsplit.push_back(jsdesc);
      }
  }
  CTransaction tx(mtx);
  txsize += GetSerializeSize(tx, SER_NETWORK, tx.nVersion);
  if (fromTaddr) {
      txsize += CTXIN_SPEND_DUST_SIZE;
      txsize += CTXOUT_REGULAR_SIZE;      // There will probably be taddr change
  }
  txsize += CTXOUT_REGULAR_SIZE * taddrRecipients.size();
  if (txsize > max_tx_size) {
      throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Too many outputs, size of raw transaction would be larger than limit of %d bytes", max_tx_size ));
  }

  // Minimum confirmations
  int nMinDepth = 1;

  // Fee in Zatoshis, not currency format)
  CAmount nFee        = ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE;
  CAmount nDefaultFee = nFee;

  // Builder (used if Sapling addresses are involved)
  boost::optional<TransactionBuilder> builder;
  if (noSproutAddrs) {
      builder = TransactionBuilder(Params().GetConsensus(), nextBlockHeight, pwalletMain);
  }

  // Contextual transaction we will build on
  // (used if no Sapling addresses are involved)
  CMutableTransaction contextualTx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), nextBlockHeight);
  bool isShielded = !fromTaddr || zaddrRecipients.size() > 0;
  if (contextualTx.nVersion == 1 && isShielded) {
      contextualTx.nVersion = 2; // Tx format should support vjoinsplits
  }

  // Create operation and add to global queue
  std::shared_ptr<AsyncRPCQueue> q = getAsyncRPCQueue();
  std::shared_ptr<AsyncRPCOperation> operation( new AsyncRPCOperation_sendmany(builder, contextualTx, fromaddress, taddrRecipients, zaddrRecipients, nMinDepth, nFee, UniValue()) );
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
  std::set<libzcash::PaymentAddress> zaddrs = {};

  bool fIncludeWatchonly = false;

  LOCK2(cs_main, pwalletMain->cs_wallet);

  // User did not provide zaddrs, so use default i.e. all addresses
  std::set<libzcash::SproutPaymentAddress> sproutzaddrs = {};
  pwalletMain->GetSproutPaymentAddresses(sproutzaddrs);

  // Sapling support
  std::set<libzcash::SaplingPaymentAddress> saplingzaddrs = {};
  pwalletMain->GetSaplingPaymentAddresses(saplingzaddrs);

  zaddrs.insert(sproutzaddrs.begin(), sproutzaddrs.end());
  zaddrs.insert(saplingzaddrs.begin(), saplingzaddrs.end());


  UniValue results(UniValue::VARR);

  if (zaddrs.size() > 0) {
      std::vector<CSproutNotePlaintextEntry> sproutEntries;
      std::vector<SaplingNoteEntry> saplingEntries;
      pwalletMain->GetFilteredNotes(sproutEntries, saplingEntries, zaddrs, nMinDepth, nMaxDepth, true, !fIncludeWatchonly, false);
      std::set<std::pair<PaymentAddress, uint256>> nullifierSet = pwalletMain->GetNullifiersForAddresses(zaddrs);

      for (auto & entry : sproutEntries) {
          ListUnspentElementT element;
          element.confirmations = entry.confirmations;
          bool hasSproutSpendingKey = pwalletMain->HaveSproutSpendingKey(boost::get<libzcash::SproutPaymentAddress>(entry.address));
          element.spendable = hasSproutSpendingKey;
          element.address = EncodePaymentAddress(entry.address);
          element.amount = CAmount(entry.plaintext.value());
          result.push_back(element);
      }

      for (auto & entry : saplingEntries) {
          ListUnspentElementT element;
          element.confirmations = entry.confirmations;
          libzcash::SaplingIncomingViewingKey ivk;
          libzcash::SaplingFullViewingKey fvk;
          pwalletMain->GetSaplingIncomingViewingKey(boost::get<libzcash::SaplingPaymentAddress>(entry.address), ivk);
          pwalletMain->GetSaplingFullViewingKey(ivk, fvk);
          bool hasSaplingSpendingKey = pwalletMain->HaveSaplingSpendingKey(fvk);
          element.spendable = hasSaplingSpendingKey;
          element.address = EncodePaymentAddress(entry.address);
          element.amount = CAmount(entry.note.value());
          result.push_back(element);
      }
  }

  error = "";
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

  error = "";
}
