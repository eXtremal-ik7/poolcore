#include "poolcore/bitcoinRPCClient.h"

#include "poolcommon/utils.h"
#include "poolcore/clientDispatcher.h"
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

static const std::string gBalanceQuery = R"json({"method": "getbalance", "params": [] })json";
static const std::string gBalanceQueryWithImmatured = R"json({"method": "getbalance", "params": ["*", 1] })json";
static const std::string gGetWalletInfoQuery = R"json({"method": "getwalletinfo", "params": [] })json";


static inline char bin2hexLowerCaseDigit(uint8_t b)
{
  return b < 10 ? '0'+b : 'a'+b-10;
}

static inline void bin2hexLowerCase(const void *in, char *out, size_t size)
{
  const uint8_t *pIn = static_cast<const uint8_t*>(in);
  for (size_t i = 0, ie = size; i != ie; ++i) {
    out[i*2] = bin2hexLowerCaseDigit(pIn[i] >> 4);
    out[i*2+1] = bin2hexLowerCaseDigit(pIn[i] & 0xF);
  }
}

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

static inline void jsonParseFloat(const rapidjson::Value &value, const char *name, double *out, bool required, bool *validAcc) {
  if (value.HasMember(name)) {
    if (value[name].IsString())
      *out = value[name].GetFloat();
    else
      *validAcc = false;
  } else if (required) {
    *validAcc = false;
  }
}

static std::string buildPostQuery(const char *data, size_t size, const std::string &host, const std::string &basicAuth)
{
  char dataLength[16];
  xitoa(size, dataLength);

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
  if (data)
    query.append(data);
  return query;
}

static void buildPostQuery(const char *data, size_t size, const std::string &host, const std::string &basicAuth, xmstream &out)
{
  char dataLength[16];
  xitoa(size, dataLength);

  out.write("POST / HTTP/1.1\r\n");
    out.write("Host: ");
      out.write(host.data(), host.size());
      out.write("\r\n");
    out.write("Connection: keep-alive\r\n");
    out.write("Authorization: Basic ");
      out.write(basicAuth.data());
      out.write("\r\n");
    out.write("Content-Length: ");
      out.write(static_cast<const char*>(dataLength));
      out.write("\r\n");
    out.write("\r\n");
  if (data)
    out.write(data, size);
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

std::string CBitcoinRpcClient::buildSendToAddress(const std::string &destination, int64_t amount)
{
  std::string result = "{";
  std::string amountFormatted = FormatMoney(amount, CoinInfo_.RationalPartSize);

  result.append(R"_("method": "sendtoaddress", )_");
  result.append(R"_("params": [)_");
    result.push_back('\"'); result.append(destination); result.append("\",");
    result.append(FormatMoney(amount, CoinInfo_.RationalPartSize));
  result.append("]}");
}

std::string CBitcoinRpcClient::buildGetTransaction(const std::string &txId)
{
  std::string result = "{";

  result.append(R"_("method": "sendtoaddress", )_");
  result.append(R"_("params": [)_");
    result.push_back('\"'); result.append(txId); result.push_back('\"');
  result.append("]}");
}

CBitcoinRpcClient::CBitcoinRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, const char *login, const char *password) :
  CNetworkClient(threadsNum),
  WorkFetcherBase_(base), ThreadsNum_(threadsNum), CoinInfo_(coinInfo)
{
  WorkFetcher_.Client = nullptr;
  WorkFetcher_.TimerEvent = newUserEvent(base, 0, [](aioUserEvent*, void *arg) {
    static_cast<CBitcoinRpcClient*>(arg)->onWorkFetchTimeout();
  }, this);

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

  HasGetWalletInfo_ = true;

  BalanceQuery_ = buildPostQuery(gBalanceQuery.data(), gBalanceQuery.size(), HostName_, BasicAuth_);
  BalanceQueryWithImmatured_ = buildPostQuery(gBalanceQueryWithImmatured.data(), gBalanceQueryWithImmatured.size(), HostName_, BasicAuth_);
  GetWalletInfoQuery_ = buildPostQuery(gGetWalletInfoQuery.data(), gGetWalletInfoQuery.size(), HostName_, BasicAuth_);
}

CPreparedQuery *CBitcoinRpcClient::prepareBlock(const void *data, size_t size)
{
  static const std::string firstPart = R"_({"method": "submitblock", "params": [")_";
  static const std::string secondPart = R"_("]})_";
  size_t fullDataSize = firstPart.size() + size*2 + secondPart.size();

  CPreparedSubmitBlock *query = new CPreparedSubmitBlock(this);

  xmstream &stream = query->stream();
  stream.reset();
  buildPostQuery(nullptr, fullDataSize, HostName_, BasicAuth_, stream);
  stream.write(firstPart.c_str());
    query->setPayLoadOffset(stream.offsetOf());
  bin2hexLowerCase(data, stream.reserve<char>(size*2), size);
  stream.write(secondPart.c_str());

  query->Connection = nullptr;
  return query;
}

bool CBitcoinRpcClient::ioGetBalance(asyncBase *base, CNetworkClient::GetBalanceResult &result)
{
  std::unique_ptr<CConnection> connection(getConnection(base));
  if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
    return false;


  if (HasGetWalletInfo_) {
    rapidjson::Document document;
    if (ioQueryJson<rapidjson::kParseNumbersAsStringsFlag>(*connection, GetWalletInfoQuery_, document, 10000000)) {
      bool errorAcc = true;
      rapidjson::Value &value = document["result"];
      std::string balance;
      std::string immatureBalance;
      // TODO: Change json parser (need parse floats as strings)
      jsonParseString(value, "balance", balance, true, &errorAcc);
      jsonParseString(value, "immature_balance", immatureBalance, true, &errorAcc);
      if (errorAcc &&
          parseMoneyValue(balance.c_str(), CoinInfo_.RationalPartSize, &result.Balance) &&
          parseMoneyValue(immatureBalance.c_str(), CoinInfo_.RationalPartSize, &result.Immatured)) {
        return true;
      } else {
        LOG_F(WARNING, "%s %s:%u: getwalletinfo invalid format", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
        return false;
      }
    } else if (connection->ParseCtx.resultCode == 404) {
      LOG_F(WARNING, "%s %s:%u: doesn't support getwalletinfo api; recommended update your node", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
      connection.reset(getConnection(base));
      if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
        return false;
      HasGetWalletInfo_ = false;
    } else {
      return false;
    }
  }

  if (!HasGetWalletInfo_) {
    rapidjson::Document balanceValue;
    rapidjson::Document fullBalanceValue;
    if (ioQueryJson<rapidjson::kParseNumbersAsStringsFlag>(*connection, BalanceQuery_, balanceValue, 10000000) &&
        ioQueryJson<rapidjson::kParseNumbersAsStringsFlag>(*connection, BalanceQueryWithImmatured_, fullBalanceValue, 10000000)) {
      std::string balanceS;
      std::string balanceFullS;
      int64_t balanceFull;
      bool errorAcc = true;
      jsonParseString(balanceValue, "result", balanceS, true, &errorAcc);
      jsonParseString(fullBalanceValue, "result", balanceFullS, true, &errorAcc);
      if (errorAcc &&
          parseMoneyValue(balanceS.c_str(), CoinInfo_.RationalPartSize, &result.Balance) &&
          parseMoneyValue(balanceFullS.c_str(), CoinInfo_.RationalPartSize, &balanceFull)) {
        result.Immatured = balanceFull - result.Balance;
        return true;
      } else {
        LOG_F(WARNING, "%s %s:%u: getbalance invalid format", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
        return false;
      }
    }
  }

  return false;
}

bool CBitcoinRpcClient::ioSendMoney(asyncBase *base, const char *address, int64_t value, CNetworkClient::SendMoneyResult &result)
{
  std::unique_ptr<CConnection> connection(getConnection(base));
  if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
    return false;

  // call 'sendtoaddress' with 3-minute timeout
  rapidjson::Document document;
  std::string query = buildSendToAddress(address, value);
  if (!ioQueryJson(*connection, query, document, 180*1000000)) {
    result.Error = connection->LastError;
    return false;
  }

  {
    bool errorAcc = true;
    jsonParseString(document, "txid", result.TxId, true, &errorAcc);
    if (!errorAcc) {
      LOG_F(WARNING, "%s %s:%u: sendtoaddress response invalid format", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
      return false;
    }
  }

  // get fee by following gettransaction call
  // TODO: subtractfeefromamount argument support
  result.Fee = 0;
  result.Error.clear();
  result.Success = true;
  query = buildGetTransaction(result.TxId);
  if (!ioQueryJson<rapidjson::kParseNumbersAsStringsFlag>(*connection, query, document, 180*1000000)) {
    LOG_F(ERROR, "%s %s:%u: can't get transaction fee, assume fee=0", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
    return true;
  }

  {
    bool errorAcc = true;
    std::string feeValue;
    rapidjson::Value &value = document["result"];
    jsonParseString(value, "fee", feeValue, true, &errorAcc);
    if (!errorAcc || feeValue.empty() || !parseMoneyValue(feeValue.data() + 1, CoinInfo_.RationalPartSize, &result.Fee)) {
      LOG_F(ERROR, "%s %s:%u: gettransaction response invalid format", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
      return true;
    }
  }

  return true;
}

void CBitcoinRpcClient::aioSubmitBlock(asyncBase *base, CPreparedQuery *queryPtr, SumbitBlockCb callback)
{
  CPreparedSubmitBlock *query = static_cast<CPreparedSubmitBlock*>(queryPtr);
  query->Connection.reset(getConnection(base));
  query->Callback = callback;
  query->Base = base;
  aioHttpConnect(query->Connection->Client, &Address_, nullptr, 10000000, [](AsyncOpStatus status, HTTPClient *httpClient, void *arg) {
    CPreparedSubmitBlock *query = static_cast<CPreparedSubmitBlock*>(arg);
    if (status != aosSuccess) {
      delete query;
      return;
    }

    aioHttpRequest(httpClient, query->stream().data<const char>(), query->stream().sizeOf(), 180000000, httpParseDefault, &query->Connection->ParseCtx, [](AsyncOpStatus status, HTTPClient *, void *arg) {
      CPreparedSubmitBlock *query = static_cast<CPreparedSubmitBlock*>(arg);
      if (status != aosSuccess) {
        delete query;
        return;
      }

      query->client<CBitcoinRpcClient>()->submitBlockRequestCb(query);
    }, query);
  }, query);
}

void CBitcoinRpcClient::poll()
{
  socketTy S = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  aioObject *object = newSocketIo(WorkFetcherBase_, S);
  WorkFetcher_.Client = httpClientNew(WorkFetcherBase_, object);
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
    httpClientDelete(WorkFetcher_.Client);
    Dispatcher_->onWorkFetcherConnectionError();
    return;
  }

  std::string gbtQuery = buildGetBlockTemplate(WorkFetcher_.LongPollId, CoinInfo_.SegwitEnabled);
  std::string query = buildPostQuery(gbtQuery.data(), gbtQuery.size(), HostName_, BasicAuth_);
  aioHttpRequest(WorkFetcher_.Client, query.c_str(), query.size(), 10000000, httpParseDefault, &WorkFetcher_.ParseCtx, [](AsyncOpStatus status, HTTPClient*, void *arg){
    static_cast<CBitcoinRpcClient*>(arg)->onWorkFetcherIncomingData(status);
  }, this);
}

void CBitcoinRpcClient::onWorkFetcherIncomingData(AsyncOpStatus status)
{
  if (status != aosSuccess || WorkFetcher_.ParseCtx.resultCode != 200) {
    LOG_F(WARNING, "%s %s:%u: request error code: %u (http result code: %u, data: %s)",
          CoinInfo_.Name.c_str(),
          HostName_.c_str(),
          static_cast<unsigned>(htons(Address_.port)),
          static_cast<unsigned>(status),
          WorkFetcher_.ParseCtx.resultCode,
          WorkFetcher_.ParseCtx.body.data ? WorkFetcher_.ParseCtx.body.data : "<null>");
    httpClientDelete(WorkFetcher_.Client);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  rapidjson::Document document;
  document.Parse(WorkFetcher_.ParseCtx.body.data);
  if (document.HasParseError()) {
    LOG_F(WARNING, "%s %s:%u: JSON parse error", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
    httpClientDelete(WorkFetcher_.Client);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  if (!document["result"].IsObject()) {
    LOG_F(WARNING, "%s %s:%u: JSON invalid format: no result object", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
    httpClientDelete(WorkFetcher_.Client);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  auto now = std::chrono::steady_clock::now();

  int64_t height;
  std::string prevBlockHash;
  bool validAcc = true;
  rapidjson::Value &resultObject = document["result"];
  jsonParseString(resultObject, "previousblockhash", prevBlockHash, true, &validAcc);
  jsonParseInt(resultObject, "height", &height, &validAcc);
  if (!validAcc) {
    LOG_F(WARNING, "%s %s:%u: getblocktemplate invalid format", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
    httpClientDelete(WorkFetcher_.Client);
    Dispatcher_->onWorkFetcherConnectionLost();
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
      Dispatcher_->onWorkFetcherNewWork(resultObject);
    }
  } else {
    // Without long polling we send new task to miner on new block found
    if (WorkFetcher_.PreviousBlock != prevBlockHash) {
      LOG_F(INFO, "%s: new work available; previous block: %s; height: %u", CoinInfo_.Name.c_str(), prevBlockHash.c_str(), static_cast<unsigned>(height));
      Dispatcher_->onWorkFetcherNewWork(resultObject);
    }
  }

  WorkFetcher_.LastTemplateTime = now;
  WorkFetcher_.PreviousBlock = prevBlockHash;

  // Send next request
  if (!WorkFetcher_.LongPollId.empty()) {
    // With long polling send new request immediately
    std::string gbtQuery = buildGetBlockTemplate(WorkFetcher_.LongPollId, CoinInfo_.SegwitEnabled);
    std::string query = buildPostQuery(gbtQuery.data(), gbtQuery.size(), HostName_, BasicAuth_);
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
  std::string query = buildPostQuery(gbtQuery.data(), gbtQuery.size(), HostName_, BasicAuth_);
  aioHttpRequest(WorkFetcher_.Client, query.c_str(), query.size(), !WorkFetcher_.LongPollId.empty() ? 0 : 10000000, httpParseDefault, &WorkFetcher_.ParseCtx, [](AsyncOpStatus status, HTTPClient*, void *arg){
    static_cast<CBitcoinRpcClient*>(arg)->onWorkFetcherIncomingData(status);
  }, this);
}

void CBitcoinRpcClient::onClientRequestTimeout()
{

}

CBitcoinRpcClient::CConnection *CBitcoinRpcClient::getConnection(asyncBase *base)
{
  CConnection *connection = new CConnection;
  httpParseDefaultInit(&connection->ParseCtx);
  connection->Socket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  connection->Client = httpClientNew(base, newSocketIo(base, connection->Socket));
  return connection;
}
