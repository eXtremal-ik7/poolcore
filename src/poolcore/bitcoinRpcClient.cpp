#include "poolcore/bitcoinRPCClient.h"

#include "blockmaker/btc.h"
#include "poolcommon/jsonSerializer.h"
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

#include <inttypes.h>

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

static const std::string gBalanceQuery = R"json({"method": "getbalance", "params": [] })json";
static const std::string gBalanceQueryWithImmatured = R"json({"method": "getbalance", "params": ["*", 1] })json";
static const std::string gGetWalletInfoQuery = R"json({"method": "getwalletinfo", "params": [] })json";
static const std::string gGetBlockChainInfoQuery = R"json({"method": "getblockchaininfo", "params": [] })json";
static const std::string gGetInfoQuery = R"json({"method": "getinfo", "params": []})json";

static inline void jsonParseInt(const rapidjson::Value &value, const char *name, uint32_t *out, bool *validAcc) {
  if (value.HasMember(name)) {
    if (value[name].IsUint())
      *out = value[name].GetUint();
    else
      *validAcc = false;
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
    query.append(data, size);
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
      out.write(basicAuth.data(), basicAuth.size());
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
  return result;
}

std::string CBitcoinRpcClient::buildGetTransaction(const std::string &txId)
{
  std::string result = "{";

  result.append(R"_("method": "gettransaction", )_");
  result.append(R"_("params": [)_");
    result.push_back('\"'); result.append(txId); result.push_back('\"');
  result.append("]}");
  return result;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::signRawTransaction(CConnection *connection, const std::string &fundedTransaction, std::string &signedTransaction, std::string &error)
{
  xmstream postData;

  {
    JSON::Object object(postData);
    object.addString("method", HasSignRawTransactionWithWallet_ ? "signrawtransactionwithwallet" : "signrawtransaction");
    object.addField("params");
    {
      JSON::Array params(postData);
      params.addString(fundedTransaction);
      params.addField();
      {
        JSON::Array privKeys(postData);
      }
    }
  }

  {
    rapidjson::Document document;
    CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery(postData.data<const char>(), postData.sizeOf(), HostName_, BasicAuth_), document, 180*1000000);
    if (status != CNetworkClient::EStatusOk) {
      constexpr int RPC_METHOD_NOT_FOUND = -32601;
      error = connection->LastError;
      if (connection->LastErrorCode == RPC_METHOD_NOT_FOUND && HasSignRawTransactionWithWallet_) {
        HasSignRawTransactionWithWallet_ = false;
        return signRawTransaction(connection, fundedTransaction, signedTransaction, error);
      } else {
        return status;
      }
    }

    if (!document.HasMember("result") || !document["result"].IsObject())
      return CNetworkClient::EStatusProtocolError;

    rapidjson::Value &signTxResult = document["result"];
    if (!signTxResult.HasMember("hex") || !signTxResult["hex"].IsString() ||
        !signTxResult.HasMember("complete") || !signTxResult["complete"].IsBool())
      return CNetworkClient::EStatusProtocolError;

    signedTransaction = signTxResult["hex"].GetString();
    if (!signTxResult["complete"].IsTrue()) {
      // Try check error
      if (signTxResult.HasMember("error") && signTxResult["error"].IsString())
        error = signTxResult["error"].GetString();
      return EStatusUnknownError;
    }
  }

  return EStatusOk;
}

CBitcoinRpcClient::CBitcoinRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, const char *login, const char *password, bool longPollEnabled) :
  CNetworkClient(threadsNum),
  WorkFetcherBase_(base), ThreadsNum_(threadsNum), CoinInfo_(coinInfo), HasLongPoll_(longPollEnabled)
{
  WorkFetcher_.Client = nullptr;
  httpParseDefaultInit(&WorkFetcher_.ParseCtx);
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

  FullHostName_ = HostName_ + ":" + std::to_string(port);

  std::string basicAuth = login;
  basicAuth.push_back(':');
  basicAuth.append(password);
  BasicAuth_.resize(base64getEncodeLength(basicAuth.size()));
  base64Encode(BasicAuth_.data(), reinterpret_cast<uint8_t*>(basicAuth.data()), basicAuth.size());

  BalanceQuery_ = buildPostQuery(gBalanceQuery.data(), gBalanceQuery.size(), HostName_, BasicAuth_);
  BalanceQueryWithImmatured_ = buildPostQuery(gBalanceQueryWithImmatured.data(), gBalanceQueryWithImmatured.size(), HostName_, BasicAuth_);
  GetWalletInfoQuery_ = buildPostQuery(gGetWalletInfoQuery.data(), gGetWalletInfoQuery.size(), HostName_, BasicAuth_);
}

CPreparedQuery *CBitcoinRpcClient::prepareBlock(const void *data, size_t size)
{
  static const std::string firstPart = R"_({"method": "submitblock", "params": [")_";
  static const std::string secondPart = R"_("]})_";
  size_t fullDataSize = firstPart.size() + size + secondPart.size();

  CPreparedSubmitBlock *query = new CPreparedSubmitBlock(this);

  xmstream &stream = query->stream();
  stream.reset();
  buildPostQuery(nullptr, fullDataSize, HostName_, BasicAuth_, stream);
  stream.write(firstPart.c_str());
    query->setPayLoadOffset(stream.offsetOf());
  stream.write(data, size);
  stream.write(secondPart.c_str());

  query->Connection = nullptr;
  return query;
}

bool CBitcoinRpcClient::ioGetBalance(asyncBase *base, CNetworkClient::GetBalanceResult &result)
{
  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return false;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
    return false;


  if (HasGetWalletInfo_) {
    rapidjson::Document document;
    if (ioQueryJson<rapidjson::kParseNumbersAsStringsFlag>(*connection, GetWalletInfoQuery_, document, 10000000) == EStatusOk) {
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
        LOG_F(WARNING, "%s %s: getwalletinfo invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
        return false;
      }
    } else if (connection->ParseCtx.resultCode == 404) {
      LOG_F(WARNING, "%s %s: doesn't support getwalletinfo api; recommended update your node", CoinInfo_.Name.c_str(), FullHostName_.c_str());
      connection.reset(getConnection(base));
      if (!connection)
        return false;
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
    if (ioQueryJson<rapidjson::kParseNumbersAsStringsFlag>(*connection, BalanceQuery_, balanceValue, 10000000) == EStatusOk &&
        ioQueryJson<rapidjson::kParseNumbersAsStringsFlag>(*connection, BalanceQueryWithImmatured_, fullBalanceValue, 10000000) == EStatusOk) {
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
        LOG_F(WARNING, "%s %s: getbalance invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
        return false;
      }
    }
  }

  return false;
}

bool CBitcoinRpcClient::ioGetBlockConfirmations(asyncBase *base, std::vector<GetBlockConfirmationsQuery> &query)
{
  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return false;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
    return false;

  std::string jsonQuery = "[";
  if (HasGetBlockChainInfo_)
    jsonQuery.append(gGetBlockChainInfoQuery);
  else
    jsonQuery.append(gGetInfoQuery);
  for (auto &block: query) {
    char buffer[256];
    snprintf(buffer, sizeof(buffer), ", {\"method\": \"getblockhash\", \"params\": [%" PRIu64 "]}", block.Height);
    jsonQuery.append(buffer);
    block.Confirmations = -2;
  }
  jsonQuery.push_back(']');

  rapidjson::Document document;
  if (ioQueryJson(*connection, buildPostQuery(jsonQuery.data(), jsonQuery.size(), HostName_, BasicAuth_), document, 5*1000000) != EStatusOk) {
    return false;
  }

  if (!document.IsArray() ||
      document.GetArray().Size() != query.size() + 1) {
    LOG_F(WARNING, "%s %s: response invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
    return false;
  }

  // Check response to getinfo query
  uint64_t bestBlockHeight = 0;
  {
    rapidjson::Value &value = document.GetArray()[0];
    if (!value.HasMember("result") || !(value["result"].IsObject() || value["result"].IsNull())) {
      LOG_F(WARNING, "%s %s: response invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
      return false;
    }

    if (value["result"].IsNull()) {
      HasGetBlockChainInfo_ = false;
      return ioGetBlockConfirmations(base, query);
    }

    value = value["result"];
    if (!value.HasMember("blocks") || !value["blocks"].IsUint64()) {
      LOG_F(WARNING, "%s %s: response invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
      return false;
    }

    bestBlockHeight = value["blocks"].GetUint64();
  }

  // Check getblockhash responses
  for (rapidjson::SizeType i = 1, ie = document.GetArray().Size(); i != ie; ++i) {
    rapidjson::Value &value = document.GetArray()[i];
    if (!value.IsObject() || !value.HasMember("result") || !value["result"].IsString()) {
      LOG_F(WARNING, "%s %s: response invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
      return false;
    }

    query[i-1].Confirmations = query[i-1].Hash == value["result"].GetString() ? bestBlockHeight - query[i-1].Height : -1;
  }

  return true;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const int64_t value, BuildTransactionResult &result)
{
  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return CNetworkClient::EStatusNetworkError;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
    return CNetworkClient::EStatusNetworkError;

  std::string rawTransaction;
  std::string fundedTransaction;
  std::string signedTransaction;

  // createrawtransaction
  result.Value = value;
  xmstream postData;

  while (result.Value) {
    postData.reset();
    {
      JSON::Object object(postData);
      object.addString("method", "createrawtransaction");
      object.addField("params");
      {
        JSON::Array params(postData);
        params.addField();
        {
          JSON::Array inputs(postData);
        }

        params.addField();
        {
          JSON::Object mainOutput(postData);
          mainOutput.addCustom(address.c_str(), FormatMoney(result.Value, CoinInfo_.RationalPartSize));
        }
      }
    }

    {
      rapidjson::Document document;
      CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery(postData.data<const char>(), postData.sizeOf(), HostName_, BasicAuth_), document, 180*1000000);
      if (status != CNetworkClient::EStatusOk) {
        result.Error = connection->LastError;
        return status;
      }
      if (!document.HasMember("result") || !document["result"].IsString())
        return CNetworkClient::EStatusProtocolError;
      rawTransaction = document["result"].GetString();
    }

    // fundrawtransaction
    postData.reset();
    {
      JSON::Object object(postData);
      object.addString("method", "fundrawtransaction");
      object.addField("params");
      {
        JSON::Array params(postData);
        params.addString(rawTransaction);
        params.addField();
        {
          JSON::Object options(postData);
          options.addString("changeAddress", changeAddress);
        }
      }
    }

    {
      rapidjson::Document document;
      CNetworkClient::EOperationStatus status = ioQueryJson<rapidjson::kParseNumbersAsStringsFlag>(*connection, buildPostQuery(postData.data<const char>(), postData.sizeOf(), HostName_, BasicAuth_), document, 180*1000000);
      if (status != CNetworkClient::EStatusOk) {
        static constexpr int RPC_WALLET_INSUFFICIENT_FUNDS = -6;
        result.Error = connection->LastError;
        return connection->LastErrorCode == RPC_WALLET_INSUFFICIENT_FUNDS ? EStatusInsufficientFunds : status;
      }

      if (!document.HasMember("result") || !document["result"].IsObject())
        return CNetworkClient::EStatusProtocolError;

      rapidjson::Value &fundTxResult = document["result"];
      if (!fundTxResult.HasMember("hex") || !fundTxResult["hex"].IsString() ||
          !fundTxResult.HasMember("fee") || !fundTxResult["fee"].IsString())
        return CNetworkClient::EStatusProtocolError;

      fundedTransaction = fundTxResult["hex"].GetString();
      if (!parseMoneyValue(fundTxResult["fee"].GetString(), CoinInfo_.RationalPartSize, &result.Fee))
        return CNetworkClient::EStatusProtocolError;
    }

    if (result.Value + result.Fee > value) {
      result.Value -= result.Fee;
    } else {
      break;
    }
  }

  if (result.Value <= 0) {
    result.Error = "too big fee";
    return CNetworkClient::EStatusUnknownError;
  }

  // signrawtransaction
  {
    EOperationStatus status = signRawTransaction(connection.get(), fundedTransaction, result.TxData, result.Error);
    if (status != EStatusOk)
      return status;
  }

  // get transaction id
  postData.reset();
  {
    JSON::Object object(postData);
    object.addString("method", "decoderawtransaction");
    object.addField("params");
    {
      JSON::Array params(postData);
      params.addString(result.TxData);
    }
  }

  {
    rapidjson::Document document;
    CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery(postData.data<const char>(), postData.sizeOf(), HostName_, BasicAuth_), document, 180*1000000);
    if (status != CNetworkClient::EStatusOk) {
      result.Error = connection->LastError;
      return status;
    }

    if (!document.HasMember("result") || !document["result"].IsObject())
      return CNetworkClient::EStatusProtocolError;

    rapidjson::Value &decodeResult = document["result"];
    if (!decodeResult.HasMember("txid") || !decodeResult["txid"].IsString())
      return CNetworkClient::EStatusProtocolError;

    result.TxId = decodeResult["txid"].GetString();
  }

  return EStatusOk;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioSendTransaction(asyncBase *base, const std::string &txData, std::string &error)
{
  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return CNetworkClient::EStatusNetworkError;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
    return CNetworkClient::EStatusNetworkError;

  xmstream postData;
  {
    JSON::Object object(postData);
    object.addString("method", "sendrawtransaction");
    object.addField("params");
    {
      JSON::Array params(postData);
      params.addString(txData);
    }
  }

  {
    rapidjson::Document document;
    CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery(postData.data<const char>(), postData.sizeOf(), HostName_, BasicAuth_), document, 180*1000000);
    if (status != CNetworkClient::EStatusOk) {
      constexpr int RPC_VERIFY_REJECTED = -26;
      constexpr int RPC_VERIFY_ALREADY_IN_CHAIN = -27;
      error = connection->LastError;
      if (connection->LastErrorCode == RPC_VERIFY_REJECTED)
        return EStatusVerifyRejected;
      else if (connection->LastErrorCode == RPC_VERIFY_ALREADY_IN_CHAIN)
        return EStatusOk;
      else
        return status;
    }
  }

  return EStatusOk;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, std::string &error)
{
  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return CNetworkClient::EStatusNetworkError;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
    return CNetworkClient::EStatusNetworkError;

  xmstream postData;
  {
    JSON::Object object(postData);
    object.addString("method", "gettransaction");
    object.addField("params");
    {
      JSON::Array params(postData);
      params.addString(txId);
    }
  }

  {
    rapidjson::Document document;
    CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery(postData.data<const char>(), postData.sizeOf(), HostName_, BasicAuth_), document, 180*1000000);
    if (status != CNetworkClient::EStatusOk) {
      constexpr int RPC_INVALID_ADDRESS_OR_KEY = -5;
      error = connection->LastError;
      if (connection->LastErrorCode == RPC_INVALID_ADDRESS_OR_KEY)
        return EStatusInvalidAddressOrKey;
      else
        return status;
    }

    if (!document.HasMember("result") || !document["result"].IsObject())
      return CNetworkClient::EStatusProtocolError;

    rapidjson::Value &getTxResult = document["result"];
    if (!getTxResult.HasMember("confirmations") || !getTxResult["confirmations"].IsInt64())
      return CNetworkClient::EStatusProtocolError;
    *confirmations = getTxResult["confirmations"].GetInt64();
  }

  return EStatusOk;
}

void CBitcoinRpcClient::aioSubmitBlock(asyncBase *base, CPreparedQuery *queryPtr, CSubmitBlockOperation *operation)
{
  CPreparedSubmitBlock *query = static_cast<CPreparedSubmitBlock*>(queryPtr);
  query->Connection.reset(getConnection(base));
  if (!query->Connection) {
    operation->accept(false, FullHostName_, "Socket creation error");
    return;
  }
  query->Operation = operation;
  query->Base = base;
  aioHttpConnect(query->Connection->Client, &Address_, nullptr, 10000000, [](AsyncOpStatus status, HTTPClient *httpClient, void *arg) {
    CPreparedSubmitBlock *query = static_cast<CPreparedSubmitBlock*>(arg);
    if (status != aosSuccess) {
      query->Operation->accept(false, query->client<CBitcoinRpcClient>()->FullHostName_, "http connection error");
      delete query;
      return;
    }

    aioHttpRequest(httpClient, query->stream().data<const char>(), query->stream().sizeOf(), 180000000, httpParseDefault, &query->Connection->ParseCtx, [](AsyncOpStatus status, HTTPClient *, void *arg) {
      CPreparedSubmitBlock *query = static_cast<CPreparedSubmitBlock*>(arg);
      if (status != aosSuccess) {
        query->Operation->accept(false, query->client<CBitcoinRpcClient>()->FullHostName_, "http request error");
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
  WorkFetcher_.LongPollId = HasLongPoll_ ? "0000000000000000000000000000000000000000000000000000000000000000" : "";
  WorkFetcher_.WorkId = 0;
  WorkFetcher_.LastTemplateTime = std::chrono::time_point<std::chrono::steady_clock>::min();
  dynamicBufferClear(&WorkFetcher_.ParseCtx.buffer);

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
  aioHttpRequest(WorkFetcher_.Client, query.c_str(), query.size(), 60000000, httpParseDefault, &WorkFetcher_.ParseCtx, [](AsyncOpStatus status, HTTPClient*, void *arg){
    static_cast<CBitcoinRpcClient*>(arg)->onWorkFetcherIncomingData(status);
  }, this);
}

void CBitcoinRpcClient::onWorkFetcherIncomingData(AsyncOpStatus status)
{
  if (status != aosSuccess || WorkFetcher_.ParseCtx.resultCode != 200) {
    LOG_F(WARNING, "%s %s: request error code: %u (http result code: %u, data: %s)",
          CoinInfo_.Name.c_str(),
          FullHostName_.c_str(),
          static_cast<unsigned>(status),
          WorkFetcher_.ParseCtx.resultCode,
          WorkFetcher_.ParseCtx.body.data ? WorkFetcher_.ParseCtx.body.data : "<null>");
    httpClientDelete(WorkFetcher_.Client);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  std::unique_ptr<CBlockTemplate> blockTemplate(new CBlockTemplate);
  blockTemplate->Document.Parse(WorkFetcher_.ParseCtx.body.data);
  if (blockTemplate->Document.HasParseError()) {
    LOG_F(WARNING, "%s %s: JSON parse error", CoinInfo_.Name.c_str(), FullHostName_.c_str());
    httpClientDelete(WorkFetcher_.Client);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  if (!blockTemplate->Document["result"].IsObject()) {
    LOG_F(WARNING, "%s %s: JSON invalid format: no result object", CoinInfo_.Name.c_str(), FullHostName_.c_str());
    httpClientDelete(WorkFetcher_.Client);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  auto now = std::chrono::steady_clock::now();

  int64_t height = 0;
  std::string prevBlockHash;
  std::string bits;
  bool validAcc = true;
  rapidjson::Value &resultObject = blockTemplate->Document["result"];
  jsonParseString(resultObject, "previousblockhash", prevBlockHash, true, &validAcc);
  jsonParseInt(resultObject, "height", &height, &validAcc);
  jsonParseString(resultObject, "bits", bits, true, &validAcc);
  if (!validAcc || prevBlockHash.size() < 16) {
    LOG_F(WARNING, "%s %s: getblocktemplate invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
    httpClientDelete(WorkFetcher_.Client);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  if (!WorkFetcher_.LongPollId.empty()) {
    jsonParseString(resultObject, "longpollid", WorkFetcher_.LongPollId, true, &validAcc);
    if (!validAcc) {
      LOG_F(WARNING, "%s %s: does not support long poll, strongly recommended update your node", CoinInfo_.Name.c_str(), FullHostName_.c_str());
      WorkFetcher_.LongPollId.clear();
    }
  }

  // Get unique work id
  uint64_t workId = blockTemplate->UniqueWorkId = readHexBE<uint64_t>(prevBlockHash.c_str(), 16);

  blockTemplate->Difficulty = BTC::getDifficulty(strtoul(bits.c_str(), nullptr, 16));

  // Check new work available
  if (!WorkFetcher_.LongPollId.empty()) {
    // With long polling enabled now we check time since last response
    uint64_t timeInterval = std::chrono::duration_cast<std::chrono::seconds>(now - WorkFetcher_.LastTemplateTime).count();
    if (timeInterval) {
      LOG_F(INFO, "%s: new work available; previous block: %s; height: %u", CoinInfo_.Name.c_str(), prevBlockHash.c_str(), static_cast<unsigned>(height));
      Dispatcher_->onWorkFetcherNewWork(blockTemplate.release());
    }
  } else {
    // Without long polling we send new task to miner on new block found
    if (WorkFetcher_.WorkId != workId) {
      LOG_F(INFO, "%s: new work available; previous block: %s; height: %u", CoinInfo_.Name.c_str(), prevBlockHash.c_str(), static_cast<unsigned>(height));
      Dispatcher_->onWorkFetcherNewWork(blockTemplate.release());
    }
  }

  WorkFetcher_.LastTemplateTime = now;
  WorkFetcher_.WorkId = workId;

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


CBitcoinRpcClient::CConnection *CBitcoinRpcClient::getConnection(asyncBase *base)
{
  CConnection *connection = new CConnection;
  connection->Socket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  // NOTE: Linux only
  if (connection->Socket == -1) {
    LOG_F(ERROR, "Can't create socket (open file descriptors limit is over?)");
    return nullptr;
  }
  connection->Client = httpClientNew(base, newSocketIo(base, connection->Socket));
  return connection;
}
