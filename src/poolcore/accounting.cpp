#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "poolcore/accounting.h"
#include "p2p/p2p.h"
// #include "flatbuffers/flatbuffers.h"
#include "poolcommon/poolapi.h"
#include "poolcore/base58.h"
#include <stdarg.h>

#define ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE   10000

// #ifndef PRI64d
// #if defined(_MSC_VER) || defined(__MSVCRT__)
// #define PRI64d  "I64d"
// #define PRI64u  "I64u"
// #define PRI64x  "I64x"
// #else
// #define PRI64d  "lld"
// #define PRI64u  "llu"
// #define PRI64x  "llx"
// #endif
// #endif

// TODO: user crossplatform way
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


using namespace boost::filesystem;


std::string real_strprintf(const std::string &format, int dummy, ...);
#define strprintf(format, ...) real_strprintf(format, 0, __VA_ARGS__)

std::string vstrprintf(const char *format, va_list ap)
{
    char buffer[50000];
    char* p = buffer;
    int limit = sizeof(buffer);
    int ret;
    for (;;)
    {
        va_list arg_ptr;
        va_copy(arg_ptr, ap);
#ifdef WIN32
        ret = _vsnprintf(p, limit, format, arg_ptr);
#else
        ret = vsnprintf(p, limit, format, arg_ptr);
#endif
        va_end(arg_ptr);
        if (ret >= 0 && ret < limit)
            break;
        if (p != buffer)
            delete[] p;
        limit *= 2;
        p = new char[limit];
        if (p == NULL)
            throw std::bad_alloc();
    }
    std::string str(p, p+ret);
    if (p != buffer)
        delete[] p;
    return str;
}

std::string real_strprintf(const std::string &format, int dummy, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, dummy);
    std::string str = vstrprintf(format.c_str(), arg_ptr);
    va_end(arg_ptr);
    return str;
}

std::string FormatMoney(int64_t n, bool fPlus=false)
{
    // Note: not using straight sprintf here because we do NOT want
    // localized number formatting.
    int64_t n_abs = (n > 0 ? n : -n);
    int64_t quotient = n_abs/COIN;
    int64_t remainder = n_abs%COIN;
    std::string str = strprintf("%" PRId64 ".%08" PRId64, quotient, remainder);

    // Right-trim excess zeros before the decimal point:
    int nTrim = 0;
    for (int i = str.size()-1; (str[i] == '0' && isdigit(str[i-2])); --i)
        ++nTrim;
    if (nTrim)
        str.erase(str.size()-nTrim, nTrim);

    if (n < 0)
        str.insert((unsigned int)0, 1, '-');
    else if (fPlus && n > 0)
        str.insert((unsigned int)0, 1, '+');
    return str;
}

AccountingDb::AccountingDb(AccountingDb::config *cfg, p2pNode *client) :
  _cfg(*cfg),
  _client(client),
  _roundsDb(path(_cfg.dbPath / "rounds").c_str()),
  _balanceDb(path(_cfg.dbPath / "balance").c_str()),
  _foundBlocksDb(path(_cfg.dbPath / "foundBlocks").c_str()),
  _poolBalanceDb(path(_cfg.dbPath / "poolBalance").c_str()),
  _payoutDb(path(_cfg.dbPath / "payouts").c_str())
{   
  {
    unsigned counter = 0;
    _sharesFd = open((path(_cfg.dbPath) / "shares.raw").c_str(), O_RDWR | O_SYNC | O_CREAT, S_IRUSR | S_IWUSR);
    if (_sharesFd == -1)
      fprintf(stderr, "<error> can't open shares file %s (%s)\n", (path(_cfg.dbPath) / "shares.raw").c_str(), strerror(errno));
    auto fileSize = lseek(_sharesFd, 0, SEEK_END);
    lseek(_sharesFd, 0, SEEK_SET);
    if (fileSize > 0) {
      xmstream stream;
      read(_sharesFd, stream.alloc(fileSize), fileSize);
    
      stream.seekSet(0);
      while (stream.remaining()) {
        uint32_t userIdSize = stream.read<uint32_t>();
        const char *userIdData = stream.jumpOver<const char>(userIdSize);
        if (!userIdData)
          break;
        int64_t value = stream.read<int64_t>();
        if (stream.eof())
          break;
        _currentScores[std::string(userIdData, userIdSize)] += value;
        counter++;
      }  
    }
  
    fprintf(stderr, "<info>loaded %u shares from shares.raw file\n", counter);
    for (auto s: _currentScores)
      fprintf(stderr, "   * %s: %" PRId64 "\n", s.first.c_str(), s.second);
  }
  
  {
    int _payoutsFdOld = open((path(_cfg.dbPath) / "payouts.raw.old").c_str(), O_RDWR | O_SYNC | O_CREAT, S_IRUSR | S_IWUSR);
    if (_payoutsFdOld == -1)
      fprintf(stderr, "<error> can't open payouts file %s (%s)\n", (path(_cfg.dbPath) / "payouts.raw.old").c_str(), strerror(errno));  
    
    struct stat s;
    fstat(_payoutsFdOld, &s);
    auto fileSize = s.st_size;

    if (fileSize > 0) {
      xmstream stream;
      read(_payoutsFdOld, stream.alloc(fileSize), fileSize);
      
      stream.seekSet(0);
      while (stream.remaining()) {
        uint32_t userIdSize = stream.read<uint32_t>();
        const char *userIdData = stream.jumpOver<const char>(userIdSize);
        if (!userIdData)
          break;
        int64_t value = stream.read<int64_t>();
        if (stream.eof())
          break;
        _payoutQueue.push_back(payoutElement(std::string(userIdData, userIdSize), value, 0));
      }
    }
    
    ftruncate(_payoutsFdOld, 0); 
    fprintf(stderr, "<info>loaded %u payouts from payouts.raw.old file\n", (unsigned)_payoutQueue.size());
  }
  
  {
    unsigned payoutsNum = 0;
    _payoutsFd = open((path(_cfg.dbPath) / "payouts.raw").c_str(), O_RDWR | O_SYNC | O_CREAT, S_IRUSR | S_IWUSR);
    if (_payoutsFd == -1)
      fprintf(stderr, "<error> can't open payouts file %s (%s)\n", (path(_cfg.dbPath) / "payouts.raw").c_str(), strerror(errno));       
      struct stat s;
      fstat(_payoutsFd, &s);
      auto fileSize = s.st_size;    
    if (fileSize > 0) {
      xmstream stream;
      read(_payoutsFd, stream.alloc(fileSize), fileSize);
      
      stream.seekSet(0);
      while (stream.remaining()) {
        payoutElement element;
        if (!element.deserializeValue(stream))
          break;
        _payoutQueue.push_back(element);
        payoutsNum++;
      }
    }
    
    fprintf(stderr, "<info>loaded %u payouts from payouts.raw file\n", payoutsNum);
    if (payoutsNum != _payoutQueue.size())
      updatePayoutFile();
  }
  
  {
    std::unique_ptr<levelDbBase::IteratorType> It(_roundsDb.iterator());
    It->seekFirst();
    for (; It->valid(); It->next()) {
      miningRound *R = new miningRound;
      RawData data = It->value();
      if (R->deserializeValue(data.data, data.size)) {
        _allRounds.push_back(R);
        for (auto p: R->payouts) {
          if (p.queued) {
            _roundsWithPayouts.insert(R);
            break; 
          }
        }
      } else {
        fprintf(stderr, "<error>rounds db contains invalid record\n");
        delete R;
      }
    }
  
    fprintf(stderr, "<info>loaded %u rounds from db\n", (unsigned)_allRounds.size());
  }

  {
    std::unique_ptr<levelDbBase::IteratorType> It(_balanceDb.iterator());
    It->seekFirst();
    for (; It->valid(); It->next()) {
      userBalance ub;
      RawData data = It->value();
      if (ub.deserializeValue(data.data, data.size))
        _balanceMap[ub.userId] = ub;
    }
    
    fprintf(stderr, "<info>loaded %u user balance data from db\n", (unsigned)_balanceMap.size());
  }
}

void AccountingDb::updatePayoutFile()
{
  xmstream stream;
  for (auto &p: _payoutQueue)
    p.serializeValue(stream);
  
  lseek(_payoutsFd, 0, SEEK_SET);
  ftruncate(_payoutsFd, 0);  
  write(_payoutsFd, stream.data(), stream.sizeOf());
}

void AccountingDb::cleanupRounds()
{
  time_t timeLabel = time(0) - _cfg.keepRoundTime;
  auto I = _allRounds.begin();
  while (I != _allRounds.end()) {
    if ((*I)->time >= timeLabel || _roundsWithPayouts.count(*I) > 0)
      break;
    _roundsDb.deleteRow(**I);
    ++I;
  }
  
  if (I != _allRounds.begin()) {
    fprintf(stderr, "<info> delete %u old rounds\n", (unsigned)std::distance(_allRounds.begin(), I));
    _allRounds.erase(_allRounds.begin(), I);
  }
}


void AccountingDb::addShare(const Share *share)
{
  // store share in shares.raw file
  {
    xmstream stream;
    stream.write<uint32_t>(share->userId()->size());
    stream.write(share->userId()->c_str(), share->userId()->size());
    stream.write<int64_t>(share->value());
    if (write(_sharesFd, stream.data(), stream.sizeOf()) != stream.sizeOf())
      fprintf(stderr, "<error> can't save share to file (%s fd=%i), it can be lost\n", strerror(errno), _sharesFd);
  }
  
  
  // increment score
  _currentScores[share->userId()->c_str()] += share->value();
  
  // create new round for block
  if (share->isBlock()) {
    {
      // save to database
      foundBlock blk;
      blk.height = share->height();
      blk.hash = share->hash()->c_str();
      blk.time = time(0);
      blk.availableCoins = share->generatedCoins();
      blk.foundBy = share->userId()->c_str();
      _foundBlocksDb.put(blk);
    }
    
    int64_t generatedCoins = share->generatedCoins();
    if (!_cfg.poolZAddr.empty()) {
      // calculate miners fee for Z-Addr moving
      generatedCoins -= ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE;
    }
    
    int64_t feeValue = generatedCoins * _cfg.poolFee / 100;
    int64_t available = generatedCoins - feeValue;    
    fprintf(stderr, " * block height: %u, hash: %s, value: %" PRId64 ", pool fee: %" PRIu64 ", available: %" PRIu64 "\n", (unsigned)share->height(), share->hash()->c_str(), generatedCoins, feeValue, available);

    miningRound *R = new miningRound;
    R->height = share->height();
    R->blockHash = share->hash()->c_str();
    R->time = time(0);
    R->availableCoins = available;
    R->totalShareValue = 0;
    
    {
      for (auto It: _currentScores) {
        roundElement element;
        element.userId = It.first;
        element.shareValue = It.second;
        R->rounds.push_back(element);
        R->totalShareValue += element.shareValue;
      }
      
      _currentScores.clear();
    }
    
    // *** calculate payments ***
    {
      // insert round into accounting base, keep sorting by height
      auto position = std::lower_bound(
        _allRounds.begin(),
        _allRounds.end(),
        R->height,
        [](const miningRound *roundArg, unsigned heightArg) -> bool { return roundArg->height < heightArg; });
      auto insertIt = _allRounds.insert(position, R);
      std::deque<miningRound*>::reverse_iterator roundIt(++insertIt);
    
      // calculate shares for each client using last N rounds
      time_t previousRoundTime = R->time;
      int64_t totalValue = 0;
      std::list<payoutAggregate> agg;
      agg.push_back(payoutAggregate(_cfg.poolFeeAddr, 0));
      agg.front().payoutValue = feeValue;
      
      while (roundIt != _allRounds.rend() && ((R->time - previousRoundTime) < 3600)) {
        // merge sorted by userId lists
        auto currentRound = *roundIt;
        auto aggIt = agg.begin();
        auto shareIt = currentRound->rounds.begin();
        while (shareIt != currentRound->rounds.end()) {
          if (aggIt == agg.end() || shareIt->userId < aggIt->userId) {
            agg.insert(aggIt, payoutAggregate(shareIt->userId, shareIt->shareValue));
            ++shareIt;
          } else if (shareIt->userId == aggIt->userId) {
            aggIt->shareValue += shareIt->shareValue;
            ++shareIt;
            ++aggIt;
          } else {
            ++aggIt;
          }
        }
        
        previousRoundTime = (*roundIt)->time;
        totalValue += currentRound->totalShareValue;
        ++roundIt;
      }
      
      // calculate payout values for each client
      int64_t totalPayout = 0;
      fprintf(stderr, " * total share value: %" PRId64 "\n", totalValue);
      for (auto I = agg.begin(), IE = agg.end(); I != IE; ++I) {
        I->payoutValue += ((double)I->shareValue / (double)totalValue) * R->availableCoins;
        totalPayout += I->payoutValue;
        fprintf(stderr, "   * addr: %s, payout: %" PRId64 "\n", I->userId.c_str(), I->payoutValue);
      }
      
      fprintf(stderr, " * total payout: %" PRId64 "\n", totalPayout);
      
      // correct payouts for use all available coins
      if (!agg.empty()) {
        int64_t diff = totalPayout - generatedCoins;
        int64_t div = diff / (int64_t)agg.size();
        int64_t mv = diff >= 0 ? 1 : -1;
        int64_t mod = (diff > 0 ? diff : -diff) % agg.size();
        
        totalPayout = 0;
        size_t i = 0;
        for (auto I = agg.begin(), IE = agg.end(); I != IE; ++I, ++i) {
          I->payoutValue -= div;
          if (i < mod)
            I->payoutValue -= mv;
          totalPayout += I->payoutValue;
        }
        
        fprintf(stderr, " * total payout (after correct): %" PRId64 "\n", totalPayout);
      }
      
      // calculate payout delta for each user and send to queue
      // merge sorted by userId lists
      auto aggIt = agg.begin();
      auto payIt = R->payouts.begin();
      while (aggIt != agg.end()) {
        if (payIt == R->payouts.end() || aggIt->userId < payIt->userId) {
          R->payouts.insert(payIt, payoutElement(aggIt->userId, aggIt->payoutValue, aggIt->payoutValue));
          _roundsWithPayouts.insert(R);
          ++aggIt;
        } else if (aggIt->userId == payIt->userId) {
          int64_t delta = aggIt->payoutValue - payIt->payoutValue;
          if (delta) {
            payIt->queued += delta;
            _roundsWithPayouts.insert(R);
          }
          payIt->payoutValue = aggIt->payoutValue;
          ++aggIt;
          ++payIt;
        } else {
          ++payIt;
        }
      }
    }
    
    // store round to DB and clear shares map
    _roundsDb.put(*R);
    lseek(_sharesFd, 0, SEEK_SET);
    ftruncate(_sharesFd, 0);
  }
}

void AccountingDb::mergeRound(const Round *round)
{

}


void AccountingDb::checkBlockConfirmations()
{
  if (_roundsWithPayouts.empty())
    return;
  
  fprintf(stderr, "<info> Checking %u blocks for confirmations...\n", (unsigned)_roundsWithPayouts.size());  
  std::vector<miningRound*> rounds(_roundsWithPayouts.begin(), _roundsWithPayouts.end());
  std::vector<std::string> hashes;
  for (auto &r: rounds)
    hashes.push_back(r->blockHash);

  auto result = ioGetBlockByHash(_client, hashes);
  if (!result)
    return;
  
  if (hashes.size() != result->blocks.size()) {
    fprintf(stderr, "<error>: response don't contains all requested blocks\n");
    return;
  }
  
  for (size_t i = 0; i < result->blocks.size(); i++) {
    auto &block = result->blocks[i];
    miningRound *R = rounds[i];
    
    if (block->confirmations == -1) {
      fprintf(stderr, "<info>: block %u/%s marked as orphan, can't do any payout\n", (unsigned)block->height, hashes[i].c_str());
      for (auto I = R->payouts.begin(), IE = R->payouts.end(); I != IE; ++I)
        I->queued = 0;
      _roundsWithPayouts.erase(R);
      _roundsDb.put(*R);
    } else if (block->confirmations >= _cfg.requiredConfirmations) {
      fprintf(stderr, "<info>: Make payout for block %u/%s\n", (unsigned)block->height, block->hash.c_str());
      for (auto I = R->payouts.begin(), IE = R->payouts.end(); I != IE; ++I) {
        requestPayout(I->userId, I->queued);
        I->queued = 0;
      }
      _roundsWithPayouts.erase(R);
      _roundsDb.put(*R);
    }
  }
  
  updatePayoutFile();
}

void AccountingDb::makePayout()
{
  if (!_payoutQueue.empty()) {
    fprintf(stderr, "<info> Accounting: checking %u payout requests...\n", (unsigned)_payoutQueue.size());
    int64_t remaining = -1;
    unsigned index = 0;
    for (auto I = _payoutQueue.begin(), IE = _payoutQueue.end(); I != IE;) {
      if (remaining != -1 && remaining <= I->payoutValue) {
        fprintf(stderr, "<info> [%u] Accounting: no money left to pay.\n", index);
        break;
      }
      
      // Check address is valid
      CBitcoinAddress address(I->userId);
      if (!_cfg.poolZAddr.empty() && !address.IsValidForZCash()) {
        fprintf(stderr, "<info> invalid ZCash address %s\n", I->userId.c_str());
        ++I;
        continue;
      }

      auto result = ioSendMoney(_client, I->userId, I->payoutValue);
      if (!result) {
        fprintf(stderr, "<info> sendMoney failed for %s\n", I->userId.c_str());
        ++I;
        continue;
      }
      
      remaining = result->remaining;
      if (result->success) {
        fprintf(stderr,
                "<info> Accounting: [%u] %s coins sent to %s with txid %s\n",
                index,
                FormatMoney(I->payoutValue).c_str(),
                I->userId.c_str(),
                result->txid.c_str());
        payoutSuccess(I->userId, I->payoutValue, result->fee, result->txid.c_str());
        _payoutQueue.erase(I++);
      } else {
        fprintf(stderr,
                "<info> Accounting: [%u] SendMoneyToDestination FAILED: %s coins to %s because: %s\n",
                index,
                FormatMoney(I->payoutValue).c_str(),
                I->userId.c_str(),
                result->error.c_str());
        ++I;
        
        // zcash specific error
        if (result->error == "Error: This transaction requires a transaction fee of at least 0.00 because of its amount, complexity, or use of recently received funds!")
          break;
      }
      
      index++;
    }
  
    updatePayoutFile();
  }  
  
  if (!_cfg.poolZAddr.empty() && !_cfg.poolTAddr.empty()) {
    // move all to Z-Addr
    auto unspent = ioListUnspent(_client);
    if (unspent && !unspent->outs.empty()) {
      fprintf(stderr, "<info> Accounting: move %u coinbase outs to Z-Addr\n", (unsigned)unspent->outs.size());
      for (auto &out: unspent->outs) {
        if (out->address == _cfg.poolTAddr || out->amount < ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE)
          continue;
    
        ZDestinationT destination;
        destination.address = _cfg.poolZAddr;
        destination.amount = out->amount;
        if (destination.amount > ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE)
          destination.amount -= ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE;
        destination.memo = "";
        auto result = ioZSendMoney(_client, out->address.c_str(), { destination });
        if (!result || result->asyncOperationId.empty()) {
          fprintf(stderr,
                  "<error> async operation start error %s: source=%s, destination=%s, amount=%li\n",
                  !result->error.empty() ? result->error.c_str() : "<unknown error>",
                  out->address.c_str(),
                  destination.address.c_str(),
                  (long)destination.amount);
          continue;
        }
    
        fprintf(stderr,
                "<info> moving %li coins from %s to %s started (%s)\n",
                (long)destination.amount,
                out->address.c_str(),
                destination.address.c_str(),
                result->asyncOperationId.c_str());
      }
    }
  }
    
  // move Z-Addr to T-Addr
  auto zaddrBalance = ioZGetBalance(_client, _cfg.poolZAddr);
  if (zaddrBalance && zaddrBalance->balance) {
    fprintf(stderr, "<info> Accounting: move %.3lf coins to transparent address\n", zaddrBalance->balance/100000000.0);
    ZDestinationT destination;
    destination.address = _cfg.poolTAddr;
    destination.amount = zaddrBalance->balance - ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE;
    destination.memo = "";  
    auto result = ioZSendMoney(_client, _cfg.poolZAddr, { destination });
    if (result) {
      fprintf(stderr,
              "<info> moving %li coins from %s to %s started (%s)\n",
              (long)destination.amount,
              _cfg.poolZAddr.c_str(),
              _cfg.poolTAddr.c_str(),
              !result->asyncOperationId.empty() ? result->asyncOperationId.c_str() : "<none>");
    }
  }
  
  // Check consistency
  std::set<std::string> waitingForPayout;
  for (auto &p: _payoutQueue)
    waitingForPayout.insert(p.userId);  
  
  int64_t totalLost = 0;  
  for (auto &userIt: _balanceMap) {
    if (userIt.second.requested < 0) {
      userIt.second.requested = 0;
      _balanceDb.put(userIt.second);
    }
    
    if (userIt.second.requested > 0 && waitingForPayout.count(userIt.second.userId) == 0) {
      // payout lost? add to back of queue
      fprintf(stderr, "<warning> found lost payout! %s\n", userIt.second.userId.c_str());
       _payoutQueue.push_back(payoutElement(userIt.second.userId, userIt.second.requested, 0));
       totalLost = userIt.second.requested;
    }    
  }
  
  if (totalLost) {
    fprintf(stderr, "<warning> total lost: %li\n", (long)totalLost);
    updatePayoutFile();  
  }
}

void AccountingDb::checkBalance()
{  
  int64_t balance = 0;
  int64_t requestedInBalance = 0;
  int64_t requestedInQueue = 0;
  int64_t immature = 0;
  int64_t userBalance = 0;
  int64_t queued = 0;
  int64_t net = 0;

  int64_t zbalance = 0;
  if (!_cfg.poolZAddr.empty()) {
    auto result = ioZGetBalance(_client, _cfg.poolZAddr);
    if (!result)
      return;
    zbalance = result->balance;
  }
  
  auto result = ioGetBalance(_client);
  if (!result)
    return;
  
  balance = result->balance + zbalance;
  immature = result->immature;

  for (auto &userIt: _balanceMap) {
    userBalance += userIt.second.balance+userIt.second.requested;
    requestedInBalance += userIt.second.requested;
  }
  
  for (auto &p: _payoutQueue)
    requestedInQueue += p.payoutValue;
  
  for (auto &roundIt: _roundsWithPayouts) {
    for (auto &pIt: roundIt->payouts)
      queued += pIt.queued;
  }
  
  net = balance + immature - userBalance - queued;
  
  {
    poolBalance pb;
    pb.time = time(0);
    pb.balance = balance;
    pb.immature = immature;
    pb.users = userBalance;
    pb.queued = queued;
    pb.net = net;
    _poolBalanceDb.put(pb);
  }
  
  fprintf(stderr,
          "<info> accounting: balance=%s req/balance=%s req/queue=%s immature=%s users=%s queued=%s, net=%s\n",
          FormatMoney(balance).c_str(),
          FormatMoney(requestedInBalance).c_str(),
          FormatMoney(requestedInQueue).c_str(),
          FormatMoney(immature).c_str(),
          FormatMoney(userBalance).c_str(),
          FormatMoney(queued).c_str(),
          FormatMoney(net).c_str());  
 

}

void AccountingDb::requestPayout(const std::string &address, int64_t value, bool force)
{
  auto It = _balanceMap.find(address);
  if (It == _balanceMap.end())
    It = _balanceMap.insert(It, std::make_pair(address, userBalance(address, _cfg.defaultMinimalPayout)));
  
  userBalance &balance = It->second;
  balance.balance += value;
  if (force || (balance.balance >= balance.minimalPayout)) {
    _payoutQueue.push_back(payoutElement(address, balance.balance, 0));
    balance.requested += balance.balance;
    balance.balance = 0;
  }
  
  _balanceDb.put(balance);
}

void AccountingDb::payoutSuccess(const std::string &address, int64_t value, int64_t fee, const std::string &transactionId)
{
  auto It = _balanceMap.find(address);
  if (It == _balanceMap.end()) {
    fprintf(stderr, "<error> payout to unknown address %s\n", address.c_str());
    return;
  }
  
  {
    payoutRecord record;
    record.userId = address;
    record.time = time(0);
    record.value = value;
    record.transactionId = transactionId;
    _payoutDb.put(record);
  }
  
  userBalance &balance = It->second;
  balance.requested -= (value+fee);
  balance.paid += value;
  _balanceDb.put(balance);
}

void AccountingDb::queryClientBalance(p2pPeer *peer, uint32_t id, const std::string &userId)
{
  flatbuffers::FlatBufferBuilder fbb;
  
  flatbuffers::Offset<ClientInfo> offset;
  auto It = _balanceMap.find(userId);
  if (It != _balanceMap.end()) {
    const userBalance &B= It->second;
    offset = CreateClientInfo(fbb, B.balance, B.requested, B.paid, fbb.CreateString(B.name), fbb.CreateString(B.email), B.minimalPayout);
  } else {
    offset = CreateClientInfo(fbb);
  }
 
  QueryResultBuilder qrb(fbb);
  qrb.add_info(offset);
  fbb.Finish(qrb.Finish());
  aiop2pSend(peer->_base, peer->connection, 3000000, fbb.GetBufferPointer(), p2pHeader(id, p2pMsgResponse, fbb.GetSize()), 0, 0);
}

void AccountingDb::updateClientInfo(p2pPeer *peer,
                                    uint32_t id,
                                    const std::string &userId,
                                    const std::string &newName,
                                    const std::string &newEmail,
                                    int64_t newMinimalPayout)
{
  flatbuffers::FlatBufferBuilder fbb;
  auto It = _balanceMap.find(userId);
  if (It == _balanceMap.end())
    It = _balanceMap.insert(It, std::make_pair(userId, userBalance(userId, _cfg.defaultMinimalPayout)));
  auto &B = It->second;
  
  if (!newName.empty())
    B.name = newName;
  if (!newEmail.empty())
    B.email = newEmail;
  if (newMinimalPayout > 0)
    B.minimalPayout = newMinimalPayout;
  
  _balanceDb.put(B);
  QueryResultBuilder qrb(fbb);
  qrb.add_status(1);
  fbb.Finish(qrb.Finish());
  aiop2pSend(peer->_base, peer->connection, 3000000, fbb.GetBufferPointer(), p2pHeader(id, p2pMsgResponse, fbb.GetSize()), 0, 0);
}

void AccountingDb::manualPayout(p2pPeer *peer,
                                uint32_t id,
                                const std::string &userId)
{
  int result;
  flatbuffers::FlatBufferBuilder fbb;
  auto It = _balanceMap.find(userId);
  if (It != _balanceMap.end()) {
    auto &B = It->second;
    if (B.balance >= ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE)
      requestPayout(userId, 0, true);
    result = 1;
  } else {
    result = 0;
  }
  
  QueryResultBuilder qrb(fbb);
  qrb.add_status(result);
  fbb.Finish(qrb.Finish());
  aiop2pSend(peer->_base, peer->connection, 3000000, fbb.GetBufferPointer(), p2pHeader(id, p2pMsgResponse, fbb.GetSize()), 0, 0);
}

void AccountingDb::moveBalance(p2pPeer *peer,
                               uint32_t id,
                               const std::string &from,
                               const std::string &to)
{
  // Fix balance map
  flatbuffers::FlatBufferBuilder fbb;
  int result;
  auto FromIt = _balanceMap.find(from);
  if (FromIt != _balanceMap.end()) {
    auto ToIt = _balanceMap.find(to);
    if (ToIt == _balanceMap.end()) {
      userBalance newUserBalance(to, _cfg.defaultMinimalPayout);
      ToIt = _balanceMap.insert(ToIt, std::make_pair(to, newUserBalance));
    }
    
    ToIt->second.balance += FromIt->second.balance; FromIt->second.balance = 0;
    ToIt->second.requested += FromIt->second.requested; FromIt->second.requested = 0;
    ToIt->second.paid += FromIt->second.paid; FromIt->second.paid = 0;
    _balanceDb.put(FromIt->second);
    _balanceDb.put(ToIt->second);
    
    // Fix payout queue
    for (auto &p: _payoutQueue) {
      if (p.userId == from)
        p.userId = to;
    }

    result = 1;    
  } else {
    result = 0;
    fprintf(stderr, "<warning> moveBalance: source account not exists");
  }
  
  QueryResultBuilder qrb(fbb);
  qrb.add_status(result);
  fbb.Finish(qrb.Finish());
  aiop2pSend(peer->_base, peer->connection, 3000000, fbb.GetBufferPointer(), p2pHeader(id, p2pMsgResponse, fbb.GetSize()), 0, 0);
}

void AccountingDb::resendBrokenTx(p2pPeer *peer,
                                  uint32_t id,
                                  const std::string &userId)
{
  int result;
  auto It = _balanceMap.find(userId);
  if (It != _balanceMap.end()) {
    auto &B = It->second;
    auto &db = getPayoutDb();
    auto *It = db.iterator();
    It->seekFirst();
    int64_t totalPayed = 0;
    int64_t total = 0;
    unsigned count = 0;
    std::vector<payoutRecord> allRecords;
    for (; It->valid(); It->next()) {
      payoutRecord pr;
      RawData data = It->value();
      if (!pr.deserializeValue(data.data, data.size))
        break;      
      if (pr.userId == userId) {
        if (pr.transactionId == "<unknown>") {
          allRecords.push_back(pr);
          total += pr.value;
          count++;
        }
        
        totalPayed += pr.value;
      }
    }
  
    delete It;
    for (auto &p: allRecords) {
      printf("brokentx: %s %u %li\n", p.userId.c_str(), (unsigned)p.time, (long)p.value);
      B.paid -= p.value;
      _balanceDb.put(B);
      db.deleteRow(p);    
      requestPayout(p.userId, p.value);
    }
    
    if (totalPayed < B.paid) {
      int64_t difference = B.paid - totalPayed;
      fprintf(stderr, "inconsistent balance, add payout resuest for %s: %li\n", userId.c_str(), difference);
      B.paid -= difference;
      requestPayout(userId, difference);
      _balanceDb.put(B);      
    }
    
    result = 1;
  } else {
    fprintf(stderr, "<error> address not found %s\n", userId.c_str());
    return;
  }
  
  flatbuffers::FlatBufferBuilder fbb;  
  QueryResultBuilder qrb(fbb);
  qrb.add_status(result);
  fbb.Finish(qrb.Finish());
  aiop2pSend(peer->_base, peer->connection, 3000000, fbb.GetBufferPointer(), p2pHeader(id, p2pMsgResponse, fbb.GetSize()), 0, 0);  
}
                                  