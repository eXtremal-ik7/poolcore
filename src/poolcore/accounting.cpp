#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "poolcore/accounting.h"
#include "p2p/p2p.h"
#include "poolcommon/poolapi.h"
#include "poolcore/base58.h"
#include "poolcore/statistics.h"
#include "loguru.hpp"
#include <stdarg.h>
#include <poolcommon/file.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif
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
  _roundsDb(_cfg.dbPath / "rounds"),
  _balanceDb(_cfg.dbPath / "balance"),
  _foundBlocksDb(_cfg.dbPath / "foundBlocks"),
  _poolBalanceDb(_cfg.dbPath / "poolBalance"),
  _payoutDb(_cfg.dbPath / "payouts")
{   
  {
    unsigned counter = 0;
    _sharesFd.open(_cfg.dbPath / "shares.raw");
    if (!_sharesFd.isOpened())
      LOG_F(ERROR, "can't open shares file %s (%s)", (_cfg.dbPath / "shares.raw").u8string().c_str(), strerror(errno));

    auto fileSize = _sharesFd.size();
    if (fileSize > 0) {
      xmstream stream;
      _sharesFd.read(stream.reserve(fileSize), 0, fileSize);
    
      stream.seekSet(0);
      while (stream.remaining()) {
        uint32_t userIdSize = stream.read<uint32_t>();
        const char *userIdData = stream.seek<const char>(userIdSize);
        if (!userIdData)
          break;
        int64_t value = stream.read<int64_t>();
        if (stream.eof())
          break;
        _currentScores[std::string(userIdData, userIdSize)] += value;
        counter++;
      }  
    }
  
    LOG_F(INFO, "loaded %u shares from shares.raw file", counter);
    for (auto s: _currentScores)
      LOG_F(INFO, "   * %s: %" PRId64 "", s.first.c_str(), s.second);

    _sharesFd.seekSet(fileSize);
  }
  
  {
    FileDescriptor payoutsFdOld;
    payoutsFdOld.open(_cfg.dbPath / "payouts.raw.old");
    if (!payoutsFdOld.isOpened())
      LOG_F(ERROR, "can't open payouts file %s (%s)", (_cfg.dbPath / "payouts.raw.old").u8string().c_str(), strerror(errno));

    auto fileSize = payoutsFdOld.size();

    if (fileSize > 0) {
      xmstream stream;
      payoutsFdOld.read(stream.reserve(fileSize), 0, fileSize);
      
      stream.seekSet(0);
      while (stream.remaining()) {
        uint32_t userIdSize = stream.read<uint32_t>();
        const char *userIdData = stream.seek<const char>(userIdSize);
        if (!userIdData)
          break;
        int64_t value = stream.read<int64_t>();
        if (stream.eof())
          break;
        _payoutQueue.push_back(payoutElement(std::string(userIdData, userIdSize), value, 0));
      }
    }

    payoutsFdOld.truncate(0);
    LOG_F(INFO, "loaded %u payouts from payouts.raw.old file", (unsigned)_payoutQueue.size());
  }
  
  {
    unsigned payoutsNum = 0;
    _payoutsFd.open(_cfg.dbPath / "payouts.raw");
    if (!_payoutsFd.isOpened())
      LOG_F(ERROR, "can't open payouts file %s (%s)", (_cfg.dbPath / "payouts.raw").u8string().c_str(), strerror(errno));

    auto fileSize = _payoutsFd.size();
    if (fileSize > 0) {
      xmstream stream;
      _payoutsFd.read(stream.reserve(fileSize), 0, fileSize);
      
      stream.seekSet(0);
      while (stream.remaining()) {
        payoutElement element;
        if (!element.deserializeValue(stream))
          break;
        _payoutQueue.push_back(element);
        payoutsNum++;
      }
    }
    
    LOG_F(INFO, "loaded %u payouts from payouts.raw file", payoutsNum);
    if (payoutsNum != _payoutQueue.size())
      updatePayoutFile();
  }
  
  {
    std::unique_ptr<rocksdbBase::IteratorType> It(_roundsDb.iterator());
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
        LOG_F(ERROR, "rounds db contains invalid record");
        delete R;
      }
    }
  
    LOG_F(INFO, "loaded %u rounds from db", (unsigned)_allRounds.size());
  }

  {
    std::unique_ptr<rocksdbBase::IteratorType> It(_balanceDb.iterator());
    It->seekFirst();
    for (; It->valid(); It->next()) {
      UserBalanceRecord ub;
      RawData data = It->value();
      if (ub.deserializeValue(data.data, data.size))
        _balanceMap[ub.Login] = ub;
    }
    
    LOG_F(INFO, "loaded %u user balance data from db", (unsigned)_balanceMap.size());
  }
}

void AccountingDb::updatePayoutFile()
{
  xmstream stream;
  for (auto &p: _payoutQueue)
    p.serializeValue(stream);
  
  _payoutsFd.write(stream.data(), 0, stream.sizeOf());
  _payoutsFd.truncate(stream.sizeOf());
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
    LOG_F(INFO, "delete %u old rounds", (unsigned)std::distance(_allRounds.begin(), I));
    _allRounds.erase(_allRounds.begin(), I);
  }
}


void AccountingDb::addShare(const Share *share, const StatisticDb *statistic)
{
  // store share in shares.raw file
  {
    xmstream stream;
    stream.write<uint32_t>(share->userId()->size());
    stream.write(share->userId()->c_str(), share->userId()->size());
    stream.write<int64_t>(share->value());
    if (_sharesFd.write(stream.data(), stream.sizeOf() != stream.sizeOf()))
      LOG_F(ERROR, "can't save share to file (%s fd=%i), it can be lost", strerror(errno), _sharesFd);
  }
  
  // increment score
  _currentScores[share->userId()->c_str()] += share->value();
  
  // create new round for block
  if (share->isBlock()) {
    {
      // save to database
      FoundBlockRecord blk;
      blk.Height = share->height();
      blk.Hash = share->hash()->c_str();
      blk.Time = time(0);
      blk.AvailableCoins = share->generatedCoins();
      blk.FoundBy = share->userId()->c_str();
      _foundBlocksDb.put(blk);
    }
    
    int64_t generatedCoins = share->generatedCoins();
    if (!_cfg.poolZAddr.empty()) {
      // calculate miners fee for Z-Addr moving
      generatedCoins -= (2*ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE);
    }
    
    int64_t feeValue = generatedCoins * _cfg.poolFee / 100;
    int64_t available = generatedCoins - feeValue;    
    LOG_F(INFO, " * block height: %u, hash: %s, value: %" PRId64 ", pool fee: %" PRIu64 ", available: %" PRIu64 "", (unsigned)share->height(), share->hash()->c_str(), generatedCoins, feeValue, available);

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
      LOG_F(INFO, " * total share value: %" PRId64 "", totalValue);
      for (auto I = agg.begin(), IE = agg.end(); I != IE; ++I) {
        I->payoutValue += ((double)I->shareValue / (double)totalValue) * R->availableCoins;
        totalPayout += I->payoutValue;

        // check correlation of share percent with power percent
        uint64_t power = statistic->getClientPower(I->userId);
        uint64_t poolPower = statistic->getPoolPower();
        if (power && poolPower) {
          double sharePercent = ((double)I->shareValue / (double)totalValue) * 100.0;
          double powerPercent = (double)power / (double)poolPower * 100.0;
          LOG_F(INFO, "   * addr: %s, payout: %" PRId64 " shares=%.3lf%% power=%.3lf%%", I->userId.c_str(), I->payoutValue, sharePercent, powerPercent);
        } else {
          LOG_F(INFO, "   * addr: %s, payout: %" PRId64 "", I->userId.c_str(), I->payoutValue);
        }
      }
      
      LOG_F(INFO, " * total payout: %" PRId64 "", totalPayout);
      
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
        
        LOG_F(INFO, " * total payout (after correct): %" PRId64 "", totalPayout);
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
    _sharesFd.truncate(0);
  }
}

void AccountingDb::mergeRound(const Round *round)
{

}


void AccountingDb::checkBlockConfirmations()
{
  if (_roundsWithPayouts.empty())
    return;
  
  LOG_F(INFO, "Checking %u blocks for confirmations...", (unsigned)_roundsWithPayouts.size());
  std::vector<miningRound*> rounds(_roundsWithPayouts.begin(), _roundsWithPayouts.end());
  std::vector<std::string> hashes;
  for (auto &r: rounds)
    hashes.push_back(r->blockHash);

  auto result = ioGetBlockByHash(_client, hashes);
  if (!result)
    return;
  
  if (hashes.size() != result->blocks.size()) {
    LOG_F(ERROR, "response don't contains all requested blocks");
    return;
  }
  
  for (size_t i = 0; i < result->blocks.size(); i++) {
    auto &block = result->blocks[i];
    miningRound *R = rounds[i];
    
    if (block->confirmations == -1) {
      LOG_F(INFO, "block %u/%s marked as orphan, can't do any payout", (unsigned)block->height, hashes[i].c_str());
      for (auto I = R->payouts.begin(), IE = R->payouts.end(); I != IE; ++I)
        I->queued = 0;
      _roundsWithPayouts.erase(R);
      _roundsDb.put(*R);
    } else if (block->confirmations >= _cfg.requiredConfirmations) {
      LOG_F(INFO, "Make payout for block %u/%s", (unsigned)block->height, block->hash.c_str());
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
    LOG_F(INFO, "Accounting: checking %u payout requests...", (unsigned)_payoutQueue.size());
    
    // Merge small payouts and payouts to invalid address
    {
      std::map<std::string, int64_t> payoutAccMap;
      for (auto I = _payoutQueue.begin(), IE = _payoutQueue.end(); I != IE;) {
        if (I->payoutValue < _cfg.minimalPayout ||
            (_cfg.checkAddressProc && !_cfg.checkAddressProc(I->userId.c_str())) ) {
          payoutAccMap[I->userId] += I->payoutValue;
          LOG_F(INFO,
                "Accounting: merge payout %s for %s (total already %s)",
                FormatMoney(I->payoutValue).c_str(),
                I->userId.c_str(),
                FormatMoney(payoutAccMap[I->userId]).c_str());
          _payoutQueue.erase(I++);        
        } else {
          ++I;
        }
      }
    
      for (auto I: payoutAccMap)
        _payoutQueue.push_back(payoutElement(I.first, I.second, 0));
    }
    
    int64_t remaining = -1;
    unsigned index = 0;
    for (auto I = _payoutQueue.begin(), IE = _payoutQueue.end(); I != IE;) {
      if (I->payoutValue < _cfg.minimalPayout) {
        LOG_F(INFO,
              "[%u] Accounting: ignore this payout to %s, value is %s, minimal is %s",
              index,
              I->userId.c_str(),
              FormatMoney(I->payoutValue).c_str(),
              FormatMoney(_cfg.minimalPayout).c_str());
        ++I;
        continue;
      }
      
      if (remaining != -1 && remaining <= I->payoutValue) {
        LOG_F(WARNING, "[%u] Accounting: no money left to pay", index);
        break;
      }
      
      // Check address is valid
      if (_cfg.checkAddressProc && !_cfg.checkAddressProc(I->userId.c_str())) {
        LOG_F(WARNING, "invalid payment address %s", I->userId.c_str());
        ++I;
        continue;
      }

      auto result = ioSendMoney(_client, I->userId, I->payoutValue);
      if (!result) {
        LOG_F(ERROR, "sendMoney failed for %s", I->userId.c_str());
        ++I;
        continue;
      }
      
      remaining = result->remaining;
      if (result->success) {
        LOG_F(INFO,
              "Accounting: [%u] %s coins sent to %s with txid %s",
              index,
              FormatMoney(I->payoutValue).c_str(),
              I->userId.c_str(),
              result->txid.c_str());
        payoutSuccess(I->userId, I->payoutValue, result->fee, result->txid.c_str());
        _payoutQueue.erase(I++);
      } else {
        LOG_F(ERROR,
              "Accounting: [%u] SendMoneyToDestination FAILED: %s coins to %s because: %s",
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
      LOG_F(INFO, "Accounting: move %u coinbase outs to Z-Addr", (unsigned)unspent->outs.size());
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
          LOG_F(INFO,
                "async operation start error %s: source=%s, destination=%s, amount=%li",
                !result->error.empty() ? result->error.c_str() : "<unknown error>",
                out->address.c_str(),
                destination.address.c_str(),
                (long)destination.amount);
          continue;
        }
    
        LOG_F(INFO,
              "moving %li coins from %s to %s started (%s)",
              (long)destination.amount,
              out->address.c_str(),
              destination.address.c_str(),
              result->asyncOperationId.c_str());
      }
    }

    
    // move Z-Addr to T-Addr
    auto zaddrBalance = ioZGetBalance(_client, _cfg.poolZAddr);
    if (zaddrBalance && zaddrBalance->balance > 0) {
      LOG_F(INFO, "<info> Accounting: move %.3lf coins to transparent address", zaddrBalance->balance/100000000.0);
      ZDestinationT destination;
      destination.address = _cfg.poolTAddr;
      destination.amount = zaddrBalance->balance - ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE;
      destination.memo = "";  
      auto result = ioZSendMoney(_client, _cfg.poolZAddr, { destination });
      if (result) {
        LOG_F(INFO,
              "moving %li coins from %s to %s started (%s)",
              (long)destination.amount,
              _cfg.poolZAddr.c_str(),
              _cfg.poolTAddr.c_str(),
              !result->asyncOperationId.empty() ? result->asyncOperationId.c_str() : "<none>");
      }
    }
  }  
  
  // Check consistency
  std::set<std::string> waitingForPayout;
  for (auto &p: _payoutQueue)
    waitingForPayout.insert(p.userId);  
  
  int64_t totalLost = 0;  
  for (auto &userIt: _balanceMap) {
    if (userIt.second.Requested > 0 && waitingForPayout.count(userIt.second.Login) == 0) {
      // payout lost? add to back of queue
      LOG_F(WARNING, "found lost payout! %s", userIt.second.Login.c_str());
       _payoutQueue.push_back(payoutElement(userIt.second.Login, userIt.second.Requested, 0));
       totalLost = userIt.second.Requested;
    }    
  }
  
  if (totalLost) {
    LOG_F(WARNING, "total lost: %li", (long)totalLost);
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
    if (!result || result->balance == -1) {
      LOG_F(ERROR, "can't get balance of Z-address %s", _cfg.poolZAddr.c_str());
      return;
    }
    zbalance = result->balance;
  }
  
  auto result = ioGetBalance(_client);
  if (!result) {
    LOG_F(ERROR, "can't retrieve balance");
    return;
  }
  
  balance = result->balance + zbalance;
  immature = result->immature;

  for (auto &userIt: _balanceMap) {
    userBalance += userIt.second.Balance+userIt.second.Requested;
    requestedInBalance += userIt.second.Requested;
  }
  
  for (auto &p: _payoutQueue)
    requestedInQueue += p.payoutValue;
  
  for (auto &roundIt: _roundsWithPayouts) {
    for (auto &pIt: roundIt->payouts)
      queued += pIt.queued;
  }
  
  net = balance + immature - userBalance - queued;
  
  {
    PoolBalanceRecord pb;
    pb.Time = time(0);
    pb.Balance = balance;
    pb.Immature = immature;
    pb.Users = userBalance;
    pb.Queued = queued;
    pb.Net = net;
    _poolBalanceDb.put(pb);
  }
  
  LOG_F(INFO,
        "accounting: balance=%s req/balance=%s req/queue=%s immature=%s users=%s queued=%s, net=%s",
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
    It = _balanceMap.insert(It, std::make_pair(address, UserBalanceRecord(address, _cfg.defaultMinimalPayout)));
  
  UserBalanceRecord &balance = It->second;
  balance.Balance += value;
  if (force || (balance.Balance >= balance.minimalPayout)) {
    _payoutQueue.push_back(payoutElement(address, balance.Balance, 0));
    balance.Requested += balance.Balance;
    balance.Balance = 0;
  }
  
  _balanceDb.put(balance);
}

void AccountingDb::payoutSuccess(const std::string &address, int64_t value, int64_t fee, const std::string &transactionId)
{
  auto It = _balanceMap.find(address);
  if (It == _balanceMap.end()) {
    LOG_F(ERROR, "payout to unknown address %s", address.c_str());
    return;
  }
  
  {
    PayoutDbRecord record;
    record.userId = address;
    record.time = time(0);
    record.value = value;
    record.transactionId = transactionId;
    _payoutDb.put(record);
  }
  
  UserBalanceRecord &balance = It->second;
  balance.Requested -= (value+fee);
  if (balance.Requested < 0) {
    balance.Balance += balance.Requested;
    balance.Requested = 0;
  }  

  balance.Paid += value;
  _balanceDb.put(balance);
}

void AccountingDb::queryClientBalance(p2pPeer *peer, uint32_t id, const std::string &userId)
{
  flatbuffers::FlatBufferBuilder fbb;
  
  flatbuffers::Offset<ClientInfo> offset;
  auto It = _balanceMap.find(userId);
  if (It != _balanceMap.end()) {
    const UserBalanceRecord &B= It->second;
    offset = CreateClientInfo(fbb, B.Balance, B.Requested, B.Paid, fbb.CreateString(B.name), fbb.CreateString(B.email), B.minimalPayout);
  } else {
    offset = CreateClientInfo(fbb);
  }
 
  QueryResultBuilder qrb(fbb);
  qrb.add_info(offset);
  fbb.Finish(qrb.Finish());
  aiop2pSend(peer->connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
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
    It = _balanceMap.insert(It, std::make_pair(userId, UserBalanceRecord(userId, _cfg.defaultMinimalPayout)));
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
  aiop2pSend(peer->connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
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
    if (B.Balance >= ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE && B.Balance >= _cfg.minimalPayout)
      requestPayout(userId, 0, true);
    result = 1;
  } else {
    result = 0;
  }
  
  QueryResultBuilder qrb(fbb);
  qrb.add_status(result);
  fbb.Finish(qrb.Finish());
  aiop2pSend(peer->connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
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
      UserBalanceRecord newUserBalance(to, _cfg.defaultMinimalPayout);
      ToIt = _balanceMap.insert(ToIt, std::make_pair(to, newUserBalance));
    }
    
    ToIt->second.Balance += FromIt->second.Balance; FromIt->second.Balance = 0;
    ToIt->second.Requested += FromIt->second.Requested; FromIt->second.Requested = 0;
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
    LOG_F(WARNING, "moveBalance: source account not exists");
  }
  
  QueryResultBuilder qrb(fbb);
  qrb.add_status(result);
  fbb.Finish(qrb.Finish());
  aiop2pSend(peer->connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
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
    std::vector<PayoutDbRecord> allRecords;
    for (; It->valid(); It->next()) {
      PayoutDbRecord pr;
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
      LOG_F(INFO, "brokentx: %s %u %li", p.userId.c_str(), (unsigned)p.time, (long)p.value);
      B.Paid -= p.value;
      _balanceDb.put(B);
      db.deleteRow(p);    
      requestPayout(p.userId, p.value);
    }
    
    if (totalPayed < B.Paid) {
      int64_t difference = B.Paid - totalPayed;
      LOG_F(WARNING, "inconsistent balance, add payout resuest for %s: %li", userId.c_str(), difference);
      B.Paid -= difference;
      requestPayout(userId, difference);
      _balanceDb.put(B);      
    }
    
    result = 1;
  } else {
    LOG_F(ERROR, "address not found %s", userId.c_str());
    return;
  }
  
  flatbuffers::FlatBufferBuilder fbb;  
  QueryResultBuilder qrb(fbb);
  qrb.add_status(result);
  fbb.Finish(qrb.Finish());
  aiop2pSend(peer->connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}
                                  
