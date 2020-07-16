#include "poolcore/bitcoinRPCClient.h"
#include "asyncio/asyncio.h"
#include "asyncio/base64.h"
#include "asyncio/http.h"
#include "asyncio/socket.h"
#include "p2putils/strExtras.h"
#include "p2putils/uriParse.h"
#include "rapidjson/document.h"
#include "loguru.hpp"
#include <string.h>
#include <chrono>

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

static inline void jsonParseInt(const rapidjson::Value &value, const char *name, int64_t *out, bool *validAcc) {
  if (value.HasMember(name)) {
    if (value[name].IsInt64())
      *out = value[name].GetInt64();
    else
      *validAcc = false;
  }
}

static inline void jsonParseString(const rapidjson::Value &value, const char *name, std::string &out, bool required, bool *validAcc) {
  if (value.HasMember(name)) {
    if (value[name].IsString())
      out = value[name].GetString();
    else
      *validAcc = false;
  } else if (required) {
    *validAcc = false;
  }
}

static std::string buildPostQuery(const char *data, const std::string &host, const std::string &basicAuth)
{
  char dataLength[16];
  xitoa(strlen(data), dataLength);

  std::string query = "POST / HTTP/1.1\r\n";
    query.append("Host: ");
      query.append(host);
      query.append("\r\n");
    query.append("Connection: keep-alive\r\n");
    query.append("Authorization: Basic ");
      query.append(basicAuth);
      query.append("\r\n");
    query.append("Content-Length: ");
      query.append(dataLength);
      query.append("\r\n");
    query.append("\r\n");
    query.append(data);
  return query;
}

static std::string buildGetBlockTemplate(const std::string &longPollId, bool segwitEnabled)
{
  std::string longPollParam;
  std::string rules;
  if (!longPollId.empty()) {
    longPollParam = "\"longpollid\": \"";
    longPollParam.append(longPollId);
    longPollParam.append("\"");
  }

  if (segwitEnabled) {
    rules = R"json("rules": ["segwit"])json";
  }

  std::string query = R"json({"jsonrpc": "1.0", "method": "getblocktemplate", "params": [{"capabilities": ["coinbasetxn", "workid", "coinbase/append"])json";
  if (!longPollParam.empty()) {
    query.push_back(',');
    query.append(longPollParam);
  }

  if (!rules.empty()) {
    query.push_back(',');
    query.append(rules);
  }

  query.append(R"json(}] })json");
  return query;
}

CBitcoinRpcClient::CBitcoinRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, const char *login, const char *password) :
  Base_(base), ThreadsNum_(threadsNum), CoinInfo_(coinInfo)
{
  WorkFetcher_.Client = nullptr;
  WorkFetcher_.TimerEvent = newUserEvent(base, 0, [](aioUserEvent*, void *arg){
    static_cast<CBitcoinRpcClient*>(arg)->onWorkFetchTimeout();
  }, this);

  Clients_.resize(threadsNum);
  for (auto &client: Clients_) {
    client.Client = nullptr;
    client.TimerEvent = newUserEvent(base, 0, [](aioUserEvent*, void *arg){
      static_cast<CBitcoinRpcClient*>(arg)->onClientRequestTimeout();
    }, this);
  }

  URI uri;
  std::string uriAddress = (std::string)"http://" + address;
  if (!uriParse(uriAddress.c_str(), &uri)) {
    LOG_F(ERROR, "%s: can't parse address %s", coinInfo.Name.c_str(), address);
    exit(1);
  }

  uint16_t port = uri.port ? uri.port : coinInfo.DefaultRpcPort;

  if (*login == 0 || *password == 0) {
    LOG_F(ERROR, "%s: you must set up login/password for node address %s", coinInfo.Name.c_str(), address);
    exit(1);
  }

  if (!uri.domain.empty()) {
    struct hostent *host = gethostbyname(uri.domain.c_str());
    if (host) {
      struct in_addr **hostAddrList = (struct in_addr**)host->h_addr_list;
      if (hostAddrList[0]) {
        Address_.ipv4 = hostAddrList[0]->s_addr;
        Address_.port = htons(port);
        Address_.family = AF_INET;
      } else {
        LOG_F(ERROR, "%s: can't lookup address %s\n", coinInfo.Name.c_str(), uri.domain.c_str());
        exit(1);
      }
    }

    HostName_ = uri.domain;
  } else {
    Address_.ipv4 = uri.ipv4;
    Address_.port = htons(port);
    Address_.family = AF_INET;

    struct in_addr addr;
    addr.s_addr = uri.ipv4;
    HostName_ = inet_ntoa(addr);
  }

  std::string basicAuth = login;
  basicAuth.push_back(':');
  basicAuth.append(password);
  BasicAuth_.resize(base64getEncodeLength(basicAuth.size()) + 1);
  base64Encode(BasicAuth_.data(), reinterpret_cast<uint8_t*>(basicAuth.data()), basicAuth.size());
}

bool CBitcoinRpcClient::ioGetBalance(int64_t *balance)
{

}

bool CBitcoinRpcClient::ioGetBlockTemplate(std::string &reslt)
{

}

bool CBitcoinRpcClient::ioSendMoney(const char *address, int64_t value, std::string &txid)
{

}

bool CBitcoinRpcClient::poll()
{
  HostAddress localAddress;
  localAddress.family = AF_INET;
  localAddress.ipv4 = INADDR_ANY;
  localAddress.port = 0;

  socketTy S = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  if (socketBind(S, &localAddress) != 0) {
    LOG_F(ERROR, "Can't bind socket to local address, wait for 10 seconds");
    socketClose(S);
    return false;
  }

  aioObject *object = newSocketIo(Base_, S);
  WorkFetcher_.Client = httpClientNew(Base_, object);
  WorkFetcher_.LongPollId = "0000000000000000000000000000000000000000000000000000000000000000";
  WorkFetcher_.PreviousBlock.clear();
  WorkFetcher_.LastTemplateTime = std::chrono::time_point<std::chrono::steady_clock>::min();
  httpParseDefaultInit(&WorkFetcher_.ParseCtx);

  aioHttpConnect(WorkFetcher_.Client, &Address_, nullptr, 3000000, [](AsyncOpStatus status, HTTPClient*, void *arg){
    static_cast<CBitcoinRpcClient*>(arg)->onWorkFetcherConnect(status);
  }, this);
}

void CBitcoinRpcClient::onWorkFetcherConnect(AsyncOpStatus status)
{
  if (status != aosSuccess) {
    // TODO: inform dispatcher
    return;
  }

  std::string gbtQuery = buildGetBlockTemplate(WorkFetcher_.LongPollId, CoinInfo_.SegwitEnabled);
  std::string query = buildPostQuery(gbtQuery.data(), HostName_, BasicAuth_);
  aioHttpRequest(WorkFetcher_.Client, query.c_str(), query.size(), 10000000, httpParseDefault, &WorkFetcher_.ParseCtx, [](AsyncOpStatus status, HTTPClient*, void *arg){
    static_cast<CBitcoinRpcClient*>(arg)->onWorkFetcherIncomingData(status);
  }, this);
}

void CBitcoinRpcClient::onWorkFetcherIncomingData(AsyncOpStatus status)
{
  if (status != aosSuccess || WorkFetcher_.ParseCtx.resultCode != 200) {
    // TODO: inform dispatcher
    LOG_F(WARNING, "%s %s:%u: request error code: %u (http result code: %u, data: %s)",
          CoinInfo_.Name.c_str(),
          HostName_.c_str(),
          static_cast<unsigned>(htons(Address_.port)),
          static_cast<unsigned>(status),
          WorkFetcher_.ParseCtx.resultCode,
          WorkFetcher_.ParseCtx.body.data ? WorkFetcher_.ParseCtx.body.data : "<null>");
    return;
  }

  rapidjson::Document document;
  document.Parse(WorkFetcher_.ParseCtx.body.data);
  if (document.HasParseError()) {
    LOG_F(WARNING, "%s %s:%u: JSON parse error", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
    // TODO: inform dispatcher
    return;
  }

  if (!document["result"].IsObject()) {
    LOG_F(WARNING, "%s %s:%u: JSON invalid format: no result object", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
    // TODO: inform dispatcher
    return;
  }

  auto now = std::chrono::steady_clock::now();

  int64_t height;
  std::string prevBlockHash;
  bool validAcc = true;
  const rapidjson::Value &resultObject = document["result"].GetObject();
  jsonParseString(resultObject, "previousblockhash", prevBlockHash, true, &validAcc);
  jsonParseInt(resultObject, "height", &height, &validAcc);
  if (!validAcc) {
    LOG_F(WARNING, "%s %s:%u: getblocktemplate invalid format", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
    // TODO: inform dispatcher
    return;
  }

  if (!WorkFetcher_.LongPollId.empty()) {
    jsonParseString(resultObject, "longpollid", WorkFetcher_.LongPollId, true, &validAcc);
    if (!validAcc) {
      LOG_F(WARNING, "%s %s:%u: does not support long poll, strongly recommended update your node", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
      WorkFetcher_.LongPollId.clear();
    }
  }

  // Check new work available
  if (!WorkFetcher_.LongPollId.empty()) {
    // With long polling enabled now we check time since last response
    unsigned timeInterval = std::chrono::duration_cast<std::chrono::seconds>(now - WorkFetcher_.LastTemplateTime).count();
    if (timeInterval) {
      LOG_F(INFO, "%s: new work available; previous block: %s; height: %u", CoinInfo_.Name.c_str(), prevBlockHash.c_str(), static_cast<unsigned>(height));
      // TODO: inform dispatcher
    }
  } else {
    // Without long polling we send new task to miner on new block found
    if (WorkFetcher_.PreviousBlock != prevBlockHash) {
      LOG_F(INFO, "%s: new work available; previous block: %s; height: %u", CoinInfo_.Name.c_str(), prevBlockHash.c_str(), static_cast<unsigned>(height));
      // TODO: inform dispatcher
    }
  }

  WorkFetcher_.LastTemplateTime = now;
  WorkFetcher_.PreviousBlock = prevBlockHash;

  // Send next request
  if (!WorkFetcher_.LongPollId.empty()) {
    // With long polling send new request immediately
    std::string gbtQuery = buildGetBlockTemplate(WorkFetcher_.LongPollId, CoinInfo_.SegwitEnabled);
    std::string query = buildPostQuery(gbtQuery.data(), HostName_, BasicAuth_);
    aioHttpRequest(WorkFetcher_.Client, query.c_str(), query.size(), !WorkFetcher_.LongPollId.empty() ? 0 : 10000000, httpParseDefault, &WorkFetcher_.ParseCtx, [](AsyncOpStatus status, HTTPClient*, void *arg){
      static_cast<CBitcoinRpcClient*>(arg)->onWorkFetcherIncomingData(status);
    }, this);
  } else {
    // Wait 1 second
    userEventStartTimer(WorkFetcher_.TimerEvent, 1*1000000, 1);
  }
}

void CBitcoinRpcClient::onWorkFetchTimeout()
{
  std::string gbtQuery = buildGetBlockTemplate(WorkFetcher_.LongPollId, CoinInfo_.SegwitEnabled);
  std::string query = buildPostQuery(gbtQuery.data(), HostName_, BasicAuth_);
  aioHttpRequest(WorkFetcher_.Client, query.c_str(), query.size(), !WorkFetcher_.LongPollId.empty() ? 0 : 10000000, httpParseDefault, &WorkFetcher_.ParseCtx, [](AsyncOpStatus status, HTTPClient*, void *arg){
    static_cast<CBitcoinRpcClient*>(arg)->onWorkFetcherIncomingData(status);
  }, this);
}

void CBitcoinRpcClient::onClientRequestTimeout()
{

}
