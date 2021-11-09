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

static void buildGetQuery(const std::string address, const std::string &host, xmstream &out)
{
  out.write("GET ");
    out.write(address.data(), address.size());
    out.write(" HTTP/1.1\r\n");
  out.write("Host: ");
    out.write(host.data(), host.size());
    out.write("\r\n");
  out.write("\r\n");
}

CPriceFetcher::CPriceFetcher(asyncBase *monitorBase, const CCoinInfo &coinInfo) : MonitorBase_(monitorBase), CoinInfo_(coinInfo)
{
  CurrentPrice_.store(0.0);
  if (coinInfo.CoinGeckoName.empty()) {
    LOG_F(ERROR, "PriceFetcher: %s not have at coingecko.com", coinInfo.Name.c_str());
    return;
  }

  if (coinInfo.Name == "BTC") {
    CurrentPrice_.store(1.0);
    return;
  }

  // coingecko resolve
  {
    struct hostent *host = gethostbyname("api.coingecko.com");
    if (host) {
      struct in_addr **hostAddrList = (struct in_addr**)host->h_addr_list;
      if (hostAddrList[0]) {
        Address_.ipv4 = hostAddrList[0]->s_addr;
        Address_.port = htons(443);
        Address_.family = AF_INET;
      } else {
        LOG_F(ERROR, "%s: can't lookup address %s\n", coinInfo.Name.c_str(), "coingecko.com");
        exit(1);
      }
    }
  }

  {
    std::string query = "/api/v3/simple/price?ids=bitcoin,";
    query.append(coinInfo.CoinGeckoName);
    query.append("&vs_currencies=USD");
    buildGetQuery(query, "api.coingecko.com", PreparedQuery_);
  }

  Client_ = nullptr;
  httpParseDefaultInit(&ParseCtx_);
  TimerEvent_ = newUserEvent(monitorBase, 0, [](aioUserEvent*, void *arg){
    static_cast<CPriceFetcher*>(arg)->updatePrice();
  }, this);

  updatePrice();
}

void CPriceFetcher::updatePrice()
{
  SSLSocket *object = sslSocketNew(MonitorBase_, nullptr);
  Client_ = httpsClientNew(MonitorBase_, object);
  dynamicBufferClear(&ParseCtx_.buffer);
  aioHttpConnect(Client_, &Address_, "api.coingecko.com", 3000000, [](AsyncOpStatus status, HTTPClient*, void *arg) {
    static_cast<CPriceFetcher*>(arg)->onConnect(status);
  }, this);
}

void CPriceFetcher::onConnect(AsyncOpStatus status)
{
  if (status != aosSuccess) {
    LOG_F(ERROR, "PriceFetcher(%s) connect error %i", CoinInfo_.Name.c_str(), status);
    httpClientDelete(Client_);
    userEventStartTimer(TimerEvent_, 60*1000000, 1);
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
    LOG_F(ERROR, "PriceFetcher(%s) request error %i; http code: %i", CoinInfo_.Name.c_str(), status, ParseCtx_.resultCode);
  }

  httpClientDelete(Client_);
  userEventStartTimer(TimerEvent_, 60*1000000, 1);
}

bool CPriceFetcher::processRequest(const char *data, size_t size)
{
  rapidjson::Document document;
  document.Parse(data, size);
  if (document.HasParseError() ||
      !document.HasMember(CoinInfo_.CoinGeckoName.c_str()) ||
      !document.HasMember("bitcoin"),
      !document[CoinInfo_.CoinGeckoName.c_str()].IsObject()) {
    LOG_F(ERROR, "PriceFetcher(%s) invalid response %s", CoinInfo_.Name.c_str(), data);
    return false;
  }


  rapidjson::Value &btcPrice = document["bitcoin"];
  rapidjson::Value &coinPrice = document[CoinInfo_.CoinGeckoName.c_str()];
  if (!btcPrice.HasMember("usd") || !btcPrice["usd"].IsNumber() ||
      !coinPrice.HasMember("usd") || !coinPrice["usd"].IsNumber()) {
    LOG_F(ERROR, "PriceFetcher(%s) invalid response %s", CoinInfo_.Name.c_str(), data);
    return false;
  }

  CurrentPrice_.store(coinPrice["usd"].GetDouble() / btcPrice["usd"].GetDouble());
  LOG_F(INFO, "%s: new price %.12lf", CoinInfo_.Name.c_str(), CurrentPrice_.load());
  return true;
}
