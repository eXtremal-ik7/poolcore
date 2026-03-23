#include "poolcore/priceFetcher.h"
#include "poolcommon/types.idl.h"
#include "loguru.hpp"

CPriceFetcher::CPriceFetcher(asyncBase *monitorBase,
                             std::vector<CCoinInfo> &coinInfo,
                             const std::string &coinGeckoApiKey) :
  Client_(coinGeckoApiKey.empty()
    ? "https://api.coingecko.com"
    : "https://pro-api.coingecko.com"),
  Base_(monitorBase),
  CoinInfo_(coinInfo),
  LastSuccessTime_(std::chrono::steady_clock::now())
{
  if (!Client_.isValid()) {
    CLOG_F(ERROR, "Can't lookup address {}", Client_.hostName());
    exit(1);
  }

  PollInterval_ = coinGeckoApiKey.empty() ? 300 * 1000000 : 60 * 1000000;

  Client_.setDefaultHeader("User-Agent", "poolcore/1.0");
  Client_.setDefaultHeader("Accept", "application/json");
  if (!coinGeckoApiKey.empty())
    Client_.setDefaultHeader("x-cg-pro-api-key", std::string(coinGeckoApiKey));

  CurrentPrices_.reset(new std::atomic<double>[coinInfo.size()]);
  for (size_t i = 0; i < coinInfo.size(); i++)
    CurrentPrices_[i].store(0.0);

  // Build query path
  QueryPath_ = "/api/v3/simple/price?ids=bitcoin";
  for (size_t i = 0; i < CoinInfo_.size(); i++) {
    const auto &coin = CoinInfo_[i];
    if (coin.CoinGeckoName.empty()) {
      CLOG_F(ERROR, "PriceFetcher: {} not have at coingecko.com", coin.Name);
      continue;
    }

    if (coin.CoinGeckoName == "BTC") {
      CurrentPrices_[i] = 1.0;
      CoinIndexMap_[coin.Name] = i;
      continue;
    }

    QueryPath_.push_back(',');
    QueryPath_.append(coin.CoinGeckoName);
    CoinIndexMap_[coin.Name] = i;
  }
  QueryPath_.append("&vs_currencies=USD");

  PreparedQuery_ = Client_.prepare({.Method = HttpMethod::GET, .Path = QueryPath_});

  TimerEvent_ = newUserEvent(monitorBase, 0, [](aioUserEvent*, void *arg){
    static_cast<CPriceFetcher*>(arg)->updatePrice();
  }, this);

  updatePrice();
}

double CPriceFetcher::getBtcUsd() const
{
  return BTCPrice_.load();
}

double CPriceFetcher::getPrice(const std::string &coinName) const
{
  auto I = CoinIndexMap_.find(coinName);
  return I != CoinIndexMap_.end() ? CurrentPrices_[I->second].load() : 0.0;
}

double CPriceFetcher::getPrice(size_t globalBackendIdx) const
{
  return CurrentPrices_[globalBackendIdx].load();
}

void CPriceFetcher::resetPricesIfStale()
{
  auto now = std::chrono::steady_clock::now();
  if (now - LastSuccessTime_ < StaleTimeout_)
    return;

  CLOG_F(WARNING, "PriceFetcher: no successful update for 30 minutes, resetting all prices to 0");
  BTCPrice_.store(0.0);
  for (size_t i = 0; i < CoinInfo_.size(); i++) {
    if (CoinInfo_[i].CoinGeckoName == "BTC")
      continue;
    CurrentPrices_[i].store(0.0);
  }
}

void CPriceFetcher::updatePrice()
{
  Client_.aioRequest<CCoinGeckoPrices>(
    Base_,
    PreparedQuery_,
    [this](AsyncOpStatus status, CCoinGeckoPrices response) {
      if (status == aosSuccess) {
        processResponse(response);
      } else {
        CLOG_F(ERROR, "PriceFetcher request error {}", static_cast<int>(status));
        resetPricesIfStale();
      }
      userEventStartTimer(TimerEvent_, PollInterval_, 1);
    });
}

void CPriceFetcher::processResponse(const CCoinGeckoPrices &response)
{
  char buffer[256];
  std::string priceFetcherLog = "priceFetcher: ";

  // bitcoin first
  auto btcIt = response.Data.find("bitcoin");
  if (btcIt != response.Data.end()) {
    BTCPrice_ = btcIt->second.Usd;
    snprintf(buffer, sizeof(buffer), "BTC/USD: %lf ", BTCPrice_.load());
    priceFetcherLog.append(buffer);
  }

  for (size_t i = 0; i < CoinInfo_.size(); i++) {
    const auto &coin = CoinInfo_[i];
    if (coin.Name == "BTC")
      continue;

    auto it = response.Data.find(coin.CoinGeckoName);
    if (it == response.Data.end())
      continue;

    double price = it->second.Usd;
    CurrentPrices_[i].store(price / BTCPrice_.load());
    snprintf(buffer, sizeof(buffer), "%s/USD: %lf %s/BTC: %.12lf ", coin.Name.c_str(), price, coin.Name.c_str(), price / BTCPrice_.load());
    priceFetcherLog.append(buffer);
  }

  LastSuccessTime_ = std::chrono::steady_clock::now();
  CLOG_F(INFO, "{}", priceFetcherLog);
}
