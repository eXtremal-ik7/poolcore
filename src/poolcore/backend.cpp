#include "poolcore/backend.h"
#include "poolcommon/utils.h"
#include "poolcore/backendData.h"
#include "poolcore/thread.h"
#include "poolcommon/debug.h"
#include "poolcommon/serialize.h"
#include "asyncio/coroutine.h"
#include "p2putils/xmstream.h"
#include "loguru.hpp"

static void checkConsistency(AccountingDb *accounting, const CCoinInfo &coinInfo)
{
  UInt<384> totalQueued = UInt<384>::zero();
  for (auto &p: accounting->getPayoutsQueue())
    totalQueued += p.Value;

  UInt<384> totalInBalance = UInt<384>::zero();
  auto &balanceDb = accounting->getBalanceDb();
  {
    std::unique_ptr<rocksdbBase::IteratorType> It(balanceDb.iterator());
    It->seekFirst();
    for (; It->valid(); It->next()) {
      UserBalanceRecord balance;
      RawData data = It->value();
      if (!balance.deserializeValue(data.data, data.size))
        break;

      totalInBalance += balance.Requested;
    }
  }

  LOG_F(INFO, "totalQueued: %s", FormatMoney(totalQueued, coinInfo.FractionalPartSize).c_str());
  LOG_F(INFO, "totalRequested: %s", FormatMoney(totalInBalance, coinInfo.FractionalPartSize).c_str());
}


PoolBackend::PoolBackend(asyncBase *base,
                         const PoolBackendConfig &cfg,
                         const CCoinInfo &info,
                         UserManager &userMgr,
                         CNetworkClientDispatcher &clientDispatcher,
                         CPriceFetcher &priceFetcher) :
  _base(base), _cfg(cfg), CoinInfo_(info), UserMgr_(userMgr), ClientDispatcher_(clientDispatcher),
  TaskHandler_(this, base), CheckConfirmationsTimer_(base), CheckBalanceTimer_(base)
{
  clientDispatcher.setBackend(this);

  if (CoinInfo_.PPSIncludeTransactionFees) {
    FeeEstimationService_ = std::make_unique<CFeeEstimationService>(base, clientDispatcher, CoinInfo_);
    clientDispatcher.setFeeEstimationService(FeeEstimationService_.get());
  }

  _timeout = 8*1000000;

  _statistics.reset(new StatisticDb(_base, _cfg, CoinInfo_));
  _accounting.reset(new AccountingDb(_base, _cfg, CoinInfo_, UserMgr_, ClientDispatcher_, priceFetcher));
  _accounting->setFeeEstimationService(FeeEstimationService_.get());

  ProfitSwitchCoeff_ = CoinInfo_.ProfitSwitchDefaultCoeff;

  if (CoinInfo_.HasDagFile) {
    EthDagFiles_ = new atomic_intrusive_ptr<EthashDagWrapper>[MaxEpochNum];
  }
}

void PoolBackend::start()
{
  _thread = std::thread([](PoolBackend *backend){ backend->backendMain(); }, this);
}

void PoolBackend::stop()
{
  const char *coin = CoinInfo_.Name.c_str();
  CheckConfirmationsTimer_.stop();
  CheckBalanceTimer_.stop();
  TaskHandler_.stop(coin, "PoolBackend task handler");
  _accounting->stop();
  _statistics->stop();
  if (FeeEstimationService_)
    FeeEstimationService_->stop();
  CheckConfirmationsTimer_.wait(coin, "PoolBackend check confirmations");
  CheckBalanceTimer_.wait(coin, "PoolBackend check balance");

  postQuitOperation(_base);
  _thread.join();
}

void PoolBackend::backendMain()
{
  InitializeWorkerThread();
  loguru::set_thread_name(CoinInfo_.Name.c_str());

  TaskHandler_.start();
  _accounting->start();
  _statistics->start();
  if (FeeEstimationService_)
    FeeEstimationService_->start();
  CheckConfirmationsTimer_.start([this]() {
    if (_accounting->hasDeferredReward())
      _accounting->checkBlockExtraInfo();
    else
      _accounting->checkBlockConfirmations();
  }, std::chrono::microseconds(_cfg.ConfirmationsCheckInterval));

  CheckBalanceTimer_.start([this]() {
    _accounting->payoutProcessor().checkBalance();
  }, std::chrono::microseconds(_cfg.BalanceCheckInterval), true);

  LOG_F(INFO, "<info>: Pool backend for '%s' started, mode is %s, tid=%u", CoinInfo_.Name.c_str(), _cfg.isMaster ? "MASTER" : "SLAVE", GetGlobalThreadId());
  checkConsistency(_accounting.get(), CoinInfo_);
  asyncLoop(_base);
}


void PoolBackend::onUserWorkSummary(const CUserWorkSummaryBatch &batch)
{
  _accounting->onUserWorkSummary(batch);
}

void PoolBackend::onWorkSummary(const CWorkSummaryBatch &batch)
{
  _statistics->onWorkSummary(batch);
}

void PoolBackend::onBlockFound(const CBlockFoundData &block)
{
  _accounting->onBlockFound(block);
}

void PoolBackend::onUserSettingsUpdate(const UserSettingsRecord &settings)
{
  _accounting->onUserSettingsUpdate(settings);
}

void PoolBackend::onFeePlanUpdate(const std::string &feePlanId, EMiningMode mode, const std::vector<UserFeePair> &feeRecord)
{
  _accounting->onFeePlanUpdate(feePlanId, mode, feeRecord);
}

void PoolBackend::onFeePlanDelete(const std::string &feePlanId)
{
  _accounting->onFeePlanDelete(feePlanId);
}

void PoolBackend::onUserFeePlanChange(const std::string &login, const std::string &feePlanId)
{
  _accounting->onUserFeePlanChange(login, feePlanId);
}

void PoolBackend::onUpdateDag(unsigned epochNumber, bool bigEpoch)
{
  if (epochNumber+1 >= MaxEpochNum)
    return;

  if (EthDagFiles_[epochNumber].get() != nullptr && EthDagFiles_[epochNumber+1].get() != nullptr)
    return;

  if (epochNumber != 0 && EthDagFiles_[epochNumber-1].get() != nullptr) {
    LOG_F(INFO, "%s: remove DAG for epoch %u", CoinInfo_.Name.c_str(), epochNumber-1);
    EthDagFiles_[epochNumber-1].reset();
  }

  if (EthDagFiles_[epochNumber].get() == nullptr) {
    LOG_F(INFO, "%s: generate DAG for epoch %u", CoinInfo_.Name.c_str(), epochNumber);
    EthDagFiles_[epochNumber].reset(new EthashDagWrapper(epochNumber, bigEpoch));
  }

  if (EthDagFiles_[epochNumber+1].get() == nullptr) {
    LOG_F(INFO, "%s: generate DAG for epoch %u", CoinInfo_.Name.c_str(), epochNumber+1);
    EthDagFiles_[epochNumber+1].reset(new EthashDagWrapper(epochNumber+1, bigEpoch));
  }
}

void PoolBackend::queryPayouts(const std::string &user, Timestamp timeFrom, unsigned count, std::vector<PayoutDbRecord> &payouts)
{
  auto &db = accountingDb()->getPayoutDb();
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());

  PayoutDbRecord valueRecord;
  xmstream resumeKey;
  auto validPredicate = [&user](const PayoutDbRecord &record) -> bool {
    return record.UserId == user;
  };

  {
    PayoutDbRecord record;
    record.UserId = user;
    record.Time = Timestamp::max();
    record.serializeKey(resumeKey);
  }

  {
    PayoutDbRecord keyRecord;
    keyRecord.UserId = user;
    keyRecord.Time = timeFrom == Timestamp() ? Timestamp::max() : timeFrom;
    It->seekForPrev<PayoutDbRecord>(keyRecord, resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  for (unsigned i = 0; i < count; i++) {
    if (!It->valid())
      break;
    payouts.emplace_back(valueRecord);

    It->prev<PayoutDbRecord>(resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }
}
