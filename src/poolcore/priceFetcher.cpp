#include "poolcore/priceFetcher.h"
#include "asyncio/socketSSL.h"
#include "asyncio/socket.h"
#include "rapidjson/document.h"
#include "loguru.hpp"

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

static void buildGetQuery(const std::string address,
                          const std::string &host,
                          const std::string &apiKey,
                          xmstream &out)
{
  out.write("GET ");
    out.write(address.data(), address.size());
    out.write(" HTTP/1.1\r\n");
  out.write("Host: ");
    out.write(host.data(), host.size());
    out.write("\r\n");
  out.write("User-Agent: poolcore/1.0\r\n");
  out.write("Accept: application/json\r\n");
  if (!apiKey.empty()) {
    out.write("x-cg-pro-api-key: ");
    out.write(apiKey.data(), apiKey.size());
    out.write("\r\n");
  }
  out.write("\r\n");
}

CPriceFetcher::CPriceFetcher(asyncBase *monitorBase,
                             std::vector<CCoinInfo> &coinInfo,
                             const std::string &coinGeckoApiKey) :
  MonitorBase_(monitorBase),
  CoinInfo_(coinInfo),
  LastSuccessTime_(std::chrono::steady_clock::now())
{
  if (!coinGeckoApiKey.empty()) {
    ApiHost_ = "pro-api.coingecko.com";
    PollInterval_ = 60 * 1000000;
  } else {
    ApiHost_ = "api.coingecko.com";
    PollInterval_ = 300 * 1000000;
  }

  CurrentPrices_.reset(new std::atomic<double>[coinInfo.size()]);
  for (size_t i = 0; i < coinInfo.size(); i++)
    CurrentPrices_[i].store(0.0);

  // coingecko resolve
  {
    struct hostent *host = gethostbyname(ApiHost_.c_str());
    if (host) {
      struct in_addr **hostAddrList = (struct in_addr**)host->h_addr_list;
      if (hostAddrList[0]) {
        Address_.ipv4 = hostAddrList[0]->s_addr;
        Address_.port = htons(443);
        Address_.family = AF_INET;
      } else {
        CLOG_F(ERROR, "Can't lookup address {}", ApiHost_);
        exit(1);
      }
    }
  }

  {
    // build request for all coins
    std::string query = "/api/v3/simple/price?ids=bitcoin";
    for (size_t i = 0; i < CoinInfo_.size(); i++) {
      const auto &coin = CoinInfo_[i];
      if (coin.CoinGeckoName.empty()) {
        CLOG_F(ERROR, "PriceFetcher: {} not have at coingecko.com", coin.Name);
        continue;
      }

      // Query BTC/USDT rate anyway
      if (coin.CoinGeckoName == "BTC") {
        CurrentPrices_[i] = 1.0;
        continue;
      }

      query.push_back(',');
      query.append(coin.CoinGeckoName);
      CoinIndexMap_[coin.Name] = i;
    }

    query.append("&vs_currencies=USD");
    buildGetQuery(query, ApiHost_, coinGeckoApiKey, PreparedQuery_);
  }

  Client_ = nullptr;
  httpParseDefaultInit(&ParseCtx_);
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
  for (size_t i = 0; i < CoinInfo_.size(); i++)
    CurrentPrices_[i].store(0.0);
}

void CPriceFetcher::updatePrice()
{
  SSLSocket *object = sslSocketNew(MonitorBase_, nullptr);
  Client_ = httpsClientNew(MonitorBase_, object);
  dynamicBufferClear(&ParseCtx_.buffer);
  aioHttpConnect(Client_, &Address_, ApiHost_.c_str(), 3000000, [](AsyncOpStatus status, HTTPClient*, void *arg) {
    static_cast<CPriceFetcher*>(arg)->onConnect(status);
  }, this);
}

void CPriceFetcher::onConnect(AsyncOpStatus status)
{
  if (status != aosSuccess) {
    CLOG_F(ERROR, "PriceFetcher connect error {}", static_cast<int>(status));
    resetPricesIfStale();
    httpClientDelete(Client_);
    userEventStartTimer(TimerEvent_, PollInterval_, 1);
    return;
  }

  aioHttpRequest(Client_, PreparedQuery_.data<const char>(), PreparedQuery_.sizeOf(), 10*1000000, httpParseDefault, &ParseCtx_, [](AsyncOpStatus status, HTTPClient*, void *arg) {
    static_cast<CPriceFetcher*>(arg)->onRequest(status);
  }, this);
}

void CPriceFetcher::onRequest(AsyncOpStatus status)
{
  if (status == aosSuccess && ParseCtx_.resultCode == 200) {
    processRequest(ParseCtx_.body.data, ParseCtx_.body.size);
  } else {
    CLOG_F(ERROR, "PriceFetcher request error {}; http code: {}", static_cast<int>(status), ParseCtx_.resultCode);
    resetPricesIfStale();
  }

  httpClientDelete(Client_);
  userEventStartTimer(TimerEvent_, PollInterval_, 1);
}

void CPriceFetcher::processRequest(const char *data, size_t size)
{
  char buffer[256];
  std::string priceFetcherLog = "priceFetcher: ";
  rapidjson::Document document;
  document.Parse(data, size);
  if (document.HasParseError()) {
    resetPricesIfStale();
    return;
  }

  // bitcoin first
  if (document.HasMember("bitcoin") && document["bitcoin"].IsObject()) {
    rapidjson::Value &coinPrice = document["bitcoin"];
    if (coinPrice.HasMember("usd") && coinPrice["usd"].IsNumber()) {
      BTCPrice_ = coinPrice["usd"].GetDouble();
      snprintf(buffer, sizeof(buffer), "BTC/USD: %lf ", BTCPrice_.load());
      priceFetcherLog.append(buffer);
    }
  }

  for (size_t i = 0; i < CoinInfo_.size(); i++) {
    const auto &coin = CoinInfo_[i];
    if (coin.Name == "BTC")
      continue;

    if (!document.HasMember(coin.CoinGeckoName.c_str()) || !document[coin.CoinGeckoName.c_str()].IsObject())
      continue;
    rapidjson::Value &coinPrice = document[coin.CoinGeckoName.c_str()];
    if (!coinPrice.HasMember("usd") || !coinPrice["usd"].IsNumber())
      continue;

    double price = coinPrice["usd"].GetDouble();
    CurrentPrices_[i].store(price / BTCPrice_.load());
    snprintf(buffer, sizeof(buffer), "%s/USD: %lf %s/BTC: %.12lf ", coin.Name.c_str(), price, coin.Name.c_str(), price / BTCPrice_.load());
    priceFetcherLog.append(buffer);
  }

  LastSuccessTime_ = std::chrono::steady_clock::now();
  CLOG_F(INFO, "{}", priceFetcherLog);
}
