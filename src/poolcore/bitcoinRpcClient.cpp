#include "poolcore/bitcoinRPCClient.h"

#include "poolcore/blockTemplate.h"
#include "poolcore/clientDispatcher.h"
#include "poolcommon/jsonSerializer.h"
#include "poolcommon/utils.h"
#include "asyncio/asyncio.h"
#include "rapidjson/document.h"
#include "loguru.hpp"
#include <string.h>

#include <inttypes.h>

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

static std::string buildGetBlockTemplate(const std::string &longPollId, bool segwitEnabled, bool mwebEnabled)
{
  char buffer[2048];
  xmstream stream(buffer, sizeof(buffer));
  stream.reset();
  {
    JSON::Object query1(stream);
    query1.addString("jsonrpc", "1.0");
    query1.addString("method", "getblocktemplate");
    query1.addField("params");
    {
      JSON::Array paramsArray(stream);
      {
        JSON::Object paramsObjectObject(stream);
        paramsObjectObject.addField("capabilities");
        {
          JSON::Array capabilitesArray(stream);
          capabilitesArray.addString("coinbasetxn");
          capabilitesArray.addString("workid");
          capabilitesArray.addString("coinbase/append");
        }

        if (!longPollId.empty())
          paramsObjectObject.addString("longpollid", longPollId);

        if (segwitEnabled || mwebEnabled) {
          paramsObjectObject.addField("rules");
          {
            JSON::Array rulesArray(stream);
            if (segwitEnabled)
              rulesArray.addString("segwit");
            if (mwebEnabled)
              rulesArray.addString("mweb");
          }
        }
      }
    }
  }

  return std::string(stream.data<char>(), stream.sizeOf());
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::signRawTransaction(asyncBase *base, const std::string &fundedTransaction, std::string &signedTransaction, std::string &error)
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
    RpcQueryResult rpcResult;
    CNetworkClient::EOperationStatus status = ioRpcQuery(base, buildPostQuery(postData), document, 180*1000000, rpcResult);
    if (status != CNetworkClient::EStatusOk) {
      constexpr int RPC_METHOD_NOT_FOUND = -32601;
      error = rpcResult.Error;
      if (rpcResult.ErrorCode == RPC_METHOD_NOT_FOUND && HasSignRawTransactionWithWallet_) {
        HasSignRawTransactionWithWallet_ = false;
        return signRawTransaction(base, fundedTransaction, signedTransaction, error);
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

static std::string buildRpcUrl(const char *address, uint16_t defaultPort, const char *wallet)
{
  std::string url = "http://";
  url.append(address);

  // Append default port if address has none
  if (!strchr(address, ':')) {
    url.push_back(':');
    url.append(std::to_string(defaultPort));
  }

  if (wallet[0]) {
    url.append("/wallet/");
    url.append(wallet);
  }
  return url;
}

CBitcoinRpcClient::CBitcoinRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, const char *login, const char *password, const char *wallet, bool longPollEnabled) :
  CoinInfo_(coinInfo),
  RpcEndpoint_(buildRpcUrl(address, coinInfo.DefaultRpcPort, wallet).c_str()),
  WorkFetcherHttpClient_(base, buildRpcUrl(address, coinInfo.DefaultRpcPort, wallet).c_str()),
  HasLongPoll_(longPollEnabled)
{
  WorkFetcher_.TimerEvent = newUserEvent(base, 0, [](aioUserEvent*, void *arg) {
    static_cast<CBitcoinRpcClient*>(arg)->onWorkFetchTimeout();
  }, this);

  if (!RpcEndpoint_.isValid()) {
    CLOG_F(ERROR, "{}: can't resolve address {}", coinInfo.Name, address);
    exit(1);
  }

  if (*login == 0 || *password == 0) {
    CLOG_F(ERROR, "{}: you must set up login/password for node address {}", coinInfo.Name, address);
    exit(1);
  }

  FullHostName_ = RpcEndpoint_.hostName();

  RpcEndpoint_.setBasicAuth(login, password);
  WorkFetcherHttpClient_.setBasicAuth(login, password);
  WorkFetcherHttpClient_.setDefaultTimeout(0);

  GetWalletInfoRequest_ = buildPostQuery(R"json({"method": "getwalletinfo", "params": [] })json");
  BalanceRequest_ = buildPostQuery(R"json({"method": "getbalance", "params": [] })json");
  BalanceWithImmaturedRequest_ = buildPostQuery(R"json({"method": "getbalance", "params": ["*", 1] })json");

  if (!longPollEnabled) {
    std::string gbtBody = buildGetBlockTemplate("", coinInfo.SegwitEnabled, coinInfo.MWebEnabled);
    HttpRequest req;
    req.Method = HttpMethod::POST;
    req.Body = gbtBody;
    GbtRequestNoLongPoll_ = WorkFetcherHttpClient_.prepare(req);
  }
}

std::string CBitcoinRpcClient::buildPostQuery(const char *data, size_t size)
{
  HttpRequest req;
  req.Method = HttpMethod::POST;
  req.Body = std::string_view(data, size);
  return RpcEndpoint_.prepare(req);
}

std::string CBitcoinRpcClient::buildPostQuery(const std::string &jsonBody)
{
  return buildPostQuery(jsonBody.data(), jsonBody.size());
}

std::string CBitcoinRpcClient::buildPostQuery(const xmstream &postData)
{
  return buildPostQuery(postData.data<const char>(), postData.sizeOf());
}

bool CBitcoinRpcClient::ioGetBalance(asyncBase *base, CNetworkClient::GetBalanceResult &result)
{
  if (HasGetWalletInfo_) {
    rapidjson::Document document;
    RpcQueryResult rpcResult;
    if (ioRpcQuery<rapidjson::kParseNumbersAsStringsFlag>(base, GetWalletInfoRequest_, document, 10000000, rpcResult) == EStatusOk) {
      bool errorAcc = true;
      rapidjson::Value &value = document["result"];
      std::string balance;
      std::string immatureBalance;
      jsonParseString(value, "balance", balance, true, &errorAcc);
      jsonParseString(value, "immature_balance", immatureBalance, true, &errorAcc);
      if (errorAcc &&
          parseMoneyValue(balance.c_str(), CoinInfo_.FractionalPartSize, &result.Balance) &&
          parseMoneyValue(immatureBalance.c_str(), CoinInfo_.FractionalPartSize, &result.Immatured)) {
        return true;
      } else {
        CLOG_F(WARNING, "{} {}: getwalletinfo invalid format", CoinInfo_.Name, FullHostName_);
        return false;
      }
    } else if (rpcResult.HttpStatus == 404) {
      CLOG_F(WARNING, "{} {}: doesn't support getwalletinfo api; recommended update your node", CoinInfo_.Name, FullHostName_);
      HasGetWalletInfo_ = false;
    } else {
      return false;
    }
  }

  if (!HasGetWalletInfo_) {
    rapidjson::Document balanceValue;
    rapidjson::Document fullBalanceValue;
    if (ioRpcQuery<rapidjson::kParseNumbersAsStringsFlag>(base, BalanceRequest_, balanceValue, 10000000) == EStatusOk &&
        ioRpcQuery<rapidjson::kParseNumbersAsStringsFlag>(base, BalanceWithImmaturedRequest_, fullBalanceValue, 10000000) == EStatusOk) {
      std::string balanceS;
      std::string balanceFullS;
      UInt<384> balanceFull;
      bool errorAcc = true;
      jsonParseString(balanceValue, "result", balanceS, true, &errorAcc);
      jsonParseString(fullBalanceValue, "result", balanceFullS, true, &errorAcc);
      if (errorAcc &&
          parseMoneyValue(balanceS.c_str(), CoinInfo_.FractionalPartSize, &result.Balance) &&
          parseMoneyValue(balanceFullS.c_str(), CoinInfo_.FractionalPartSize, &balanceFull)) {
        result.Immatured = balanceFull - result.Balance;
        return true;
      } else {
        CLOG_F(WARNING, "{} {}: getbalance invalid format", CoinInfo_.Name, FullHostName_);
        return false;
      }
    }
    CLOG_F(INFO, "{} {}: ioGetBalance: getbalance failed", CoinInfo_.Name, FullHostName_);
  }

  return false;
}

bool CBitcoinRpcClient::ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockConfirmationsQuery> &query)
{
  for (auto &It: query)
    It.Confirmations = -2;

  static const std::string gGetBlockChainInfoQuery = R"json({"method": "getblockchaininfo", "params": [] })json";
  static const std::string gGetInfoQuery = R"json({"method": "getinfo", "params": []})json";

  std::string jsonQuery = "[";
  if (HasGetBlockChainInfo_)
    jsonQuery.append(gGetBlockChainInfoQuery);
  else
    jsonQuery.append(gGetInfoQuery);
  for (auto &block: query) {
    char buffer[256];
    snprintf(buffer, sizeof(buffer), ", {\"method\": \"getblockhash\", \"params\": [%" PRIu64 "]}", block.Height);
    jsonQuery.append(buffer);
  }
  jsonQuery.push_back(']');

  rapidjson::Document document;
  if (ioRpcQuery(base, buildPostQuery(jsonQuery), document, 5*1000000) != EStatusOk) {
    return false;
  }

  if (!document.IsArray() ||
      document.GetArray().Size() != query.size() + 1) {
    CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
    return false;
  }

  // Check response to getinfo query
  uint64_t bestBlockHeight = 0;
  {
    rapidjson::Value &value = document.GetArray()[0];
    if (!value.HasMember("result") || !(value["result"].IsObject() || value["result"].IsNull())) {
      CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
      return false;
    }

    if (value["result"].IsNull()) {
      if (HasGetBlockChainInfo_) {
        HasGetBlockChainInfo_ = false;
        return ioGetBlockConfirmations(base, orphanAgeLimit, query);
      }
      CLOG_F(WARNING,
             "{} {}: both getblockchaininfo and getinfo returned null",
             CoinInfo_.Name,
             FullHostName_);
      return false;
    }

    value = value["result"];
    if (!value.HasMember("blocks") || !value["blocks"].IsUint64()) {
      CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
      return false;
    }

    bestBlockHeight = value["blocks"].GetUint64();
  }

  // Check getblockhash responses
  for (rapidjson::SizeType i = 1, ie = document.GetArray().Size(); i != ie; ++i) {
    rapidjson::Value &value = document.GetArray()[i];
    if (!value.IsObject() || !value.HasMember("result")) {
      CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
      return false;
    }

    if (value["result"].IsString()) {
      query[i-1].Confirmations = query[i-1].Hash == value["result"].GetString() ? bestBlockHeight - query[i-1].Height : -1;
    } else {
      // getblockhash returned null (block height out of range) — treat as orphan
      query[i-1].Confirmations = -1;
    }
  }

  return true;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const UInt<384> &value, BuildTransactionResult &result)
{
  std::string rawTransaction;
  std::string fundedTransaction;

  // createrawtransaction
  result.Value = value;
  xmstream postData;

  while (result.Value.nonZero()) {
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
          mainOutput.addCustom(address.c_str(), FormatMoney(result.Value, CoinInfo_.FractionalPartSize));
        }
      }
    }

    {
      rapidjson::Document document;
      RpcQueryResult rpcResult;
      CNetworkClient::EOperationStatus status = ioRpcQuery(base, buildPostQuery(postData), document, 180*1000000, rpcResult);
      if (status != CNetworkClient::EStatusOk) {
        result.Error = rpcResult.Error;
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
        if (CoinInfo_.HasExtendedFundRawTransaction) {
          params.addField();
          {
            JSON::Object options(postData);
            options.addString("changeAddress", changeAddress);
          }
        }
      }
    }

    {
      rapidjson::Document document;
      RpcQueryResult rpcResult;
      CNetworkClient::EOperationStatus status = ioRpcQuery<rapidjson::kParseNumbersAsStringsFlag>(base, buildPostQuery(postData), document, 180*1000000, rpcResult);
      if (status != CNetworkClient::EStatusOk) {
        static constexpr int RPC_WALLET_INSUFFICIENT_FUNDS = -6;
        result.Error = rpcResult.Error;
        return rpcResult.ErrorCode == RPC_WALLET_INSUFFICIENT_FUNDS ? EStatusInsufficientFunds : status;
      }

      if (!document.HasMember("result") || !document["result"].IsObject())
        return CNetworkClient::EStatusProtocolError;

      rapidjson::Value &fundTxResult = document["result"];
      if (!fundTxResult.HasMember("hex") || !fundTxResult["hex"].IsString() ||
          !fundTxResult.HasMember("fee") || !fundTxResult["fee"].IsString())
        return CNetworkClient::EStatusProtocolError;

      fundedTransaction = fundTxResult["hex"].GetString();
      if (!parseMoneyValue(fundTxResult["fee"].GetString(), CoinInfo_.FractionalPartSize, &result.Fee))
        return CNetworkClient::EStatusProtocolError;
    }

    if (result.Value + result.Fee > value) {
      if (result.Value <= result.Fee) {
        result.Error = "too big fee";
        return CNetworkClient::EStatusUnknownError;
      }

      result.Value -= result.Fee;
    } else {
      break;
    }
  }

  // signrawtransaction
  {
    EOperationStatus status = signRawTransaction(base, fundedTransaction, result.TxData, result.Error);
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
    RpcQueryResult rpcResult;
    CNetworkClient::EOperationStatus status = ioRpcQuery(base, buildPostQuery(postData), document, 180*1000000, rpcResult);
    if (status != CNetworkClient::EStatusOk) {
      result.Error = rpcResult.Error;
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

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioSendTransaction(asyncBase *base, const std::string &txData, const std::string&, std::string &error)
{
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
    RpcQueryResult rpcResult;
    CNetworkClient::EOperationStatus status = ioRpcQuery(base, buildPostQuery(postData), document, 180*1000000, rpcResult);
    if (status != CNetworkClient::EStatusOk) {

      constexpr int RPC_VERIFY_ERROR = -25;
      constexpr int RPC_VERIFY_REJECTED = -26;
      constexpr int RPC_VERIFY_ALREADY_IN_CHAIN = -27;
      error = rpcResult.Error;
      if (rpcResult.ErrorCode == RPC_VERIFY_ERROR && rpcResult.Error == "Missing inputs")
        return EStatusVerifyRejected;
      if (rpcResult.ErrorCode == RPC_VERIFY_REJECTED)
        return EStatusVerifyRejected;
      else if (rpcResult.ErrorCode == RPC_VERIFY_ALREADY_IN_CHAIN)
        return EStatusOk;
      else
        return status;
    }
  }

  return EStatusOk;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, UInt<384> *txFee, std::string &error)
{
  // Not used here
  *txFee = 0u;

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
    RpcQueryResult rpcResult;
    CNetworkClient::EOperationStatus status = ioRpcQuery(base, buildPostQuery(postData), document, 180*1000000, rpcResult);
    if (status != CNetworkClient::EStatusOk) {
      constexpr int RPC_INVALID_ADDRESS_OR_KEY = -5;
      error = rpcResult.Error;
      if (rpcResult.ErrorCode == RPC_INVALID_ADDRESS_OR_KEY)
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

void CBitcoinRpcClient::aioSubmitBlock(asyncBase *base, const void *data, size_t size, CSubmitBlockOperation *operation)
{
  static const std::string prefix = R"_({"method": "submitblock", "params": [")_";
  static const std::string suffix = R"_("]})_";

  std::string body;
  body.reserve(prefix.size() + size + suffix.size());
  body.append(prefix);
  body.append(static_cast<const char*>(data), size);
  body.append(suffix);

  RpcEndpoint_.aioRequest(base, buildPostQuery(body),
    [this, operation](AsyncOpStatus status, HttpResponse response) {
      bool success = false;
      std::string error;

      if (status != aosSuccess) {
        error = "http request error";
      } else {
        rapidjson::Document document;
        document.Parse(response.Body.data(), response.Body.size());
        if (!document.HasParseError() && document.HasMember("result")) {
          if (document["result"].IsNull()) {
            success = true;
          } else if (document["result"].IsString()) {
            error = document["result"].GetString();
          }
        }
        if (response.StatusCode != 200 && error.empty()) {
          if (!document.HasParseError() && document.HasMember("error") && document["error"].IsObject()) {
            rapidjson::Value &errVal = document["error"];
            if (errVal.HasMember("message") && errVal["message"].IsString())
              error = errVal["message"].GetString();
          }
        }
      }

      operation->accept(success, FullHostName_, error);
    }, 180000000);
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioListUnspent(asyncBase *base, ListUnspentResult &result)
{
  xmstream postData;
  {
    JSON::Object object(postData);
    object.addString("method", "listunspent");
    object.addField("params");
    {
      JSON::Array params(postData);
    }
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioRpcQuery<rapidjson::kParseNumbersAsStringsFlag>(base, buildPostQuery(postData), document, 180*1000000);
  if (status != CNetworkClient::EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsArray())
    return CNetworkClient::EStatusProtocolError;
  rapidjson::Value::Array array = document["result"].GetArray();
  for (rapidjson::SizeType i = 0, ie = array.Size(); i != ie; ++i) {
    if (!array[i].IsObject())
      return CNetworkClient::EStatusProtocolError;
    rapidjson::Value &output = array[i];
    if (!output.HasMember("address") || !output["address"].IsString() ||
        !output.HasMember("amount") || !output["amount"].IsString() ||
        !output.HasMember("generated") || !output["generated"].IsBool())
      return CNetworkClient::EStatusProtocolError;

    ListUnspentElement &element = result.Outs.emplace_back();
    element.Address = output["address"].GetString();
    if (!parseMoneyValue(output["amount"].GetString(), CoinInfo_.FractionalPartSize, &element.Amount))
      return CNetworkClient::EStatusProtocolError;
    element.IsCoinbase = output["generated"].GetBool();
  }

  return CNetworkClient::EStatusOk;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioZSendMany(asyncBase *base, const std::string &source, const std::string &destination, const UInt<384> &amount, const std::string &memo, uint64_t minConf, const UInt<384> &fee, CNetworkClient::ZSendMoneyResult &result)
{
  xmstream postData;
  {
    JSON::Object object(postData);
    object.addString("method", "z_sendmany");
    object.addField("params");
    {
      JSON::Array params(postData);
      params.addString(source);
      params.addField();
      {
        JSON::Array dstArg(postData);
        dstArg.addField();
        {
          JSON::Object singleDstArg(postData);
          singleDstArg.addString("address", destination);
          singleDstArg.addCustom("amount", FormatMoney(amount, CoinInfo_.FractionalPartSize));
          if (!memo.empty())
            singleDstArg.addString("memo", memo);
        }
      }
      params.addInt(minConf);
      params.addCustom(FormatMoney(fee, CoinInfo_.FractionalPartSize));
    }
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioRpcQuery<rapidjson::kParseNumbersAsStringsFlag>(base, buildPostQuery(postData), document, 180*1000000);
  if (status != CNetworkClient::EStatusOk)
    return status;

  if (!document.HasMember("result"))
    return CNetworkClient::EStatusProtocolError;
  rapidjson::Value &resultValue = document["result"];
  if (resultValue.IsString()) {
    result.AsyncOperationId = resultValue.GetString();
    return CNetworkClient::EStatusOk;
  } else if (resultValue.IsNull()) {
    if (resultValue.HasMember("error") && resultValue["error"].IsObject()) {
      rapidjson::Value &errorValue = resultValue["error"];
      if (errorValue.HasMember("message") && errorValue["message"].IsString())
        result.Error = errorValue["message"].GetString();
    }
    return CNetworkClient::EStatusUnknownError;
  } else {
    return CNetworkClient::EStatusUnknownError;
  }
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioZGetBalance(asyncBase *base, const std::string &address, UInt<384> *balance)
{
  xmstream postData;
  {
    JSON::Object object(postData);
    object.addString("method", "z_getbalance");
    object.addField("params");
    {
      JSON::Array params(postData);
      params.addString(address);
    }
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioRpcQuery<rapidjson::kParseNumbersAsStringsFlag>(base, buildPostQuery(postData), document, 180*1000000);
  if (status != CNetworkClient::EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsString())
    return CNetworkClient::EStatusProtocolError;
  if (!parseMoneyValue(document["result"].GetString(), CoinInfo_.FractionalPartSize, balance))
    return CNetworkClient::EStatusProtocolError;
  return CNetworkClient::EStatusOk;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioWalletService(asyncBase*, std::string&)
{
  return CNetworkClient::EStatusOk;
}

bool CBitcoinRpcClient::ioGetBlockTxFees(asyncBase *base,
                                         int64_t fromHeight,
                                         int64_t toHeight,
                                         std::vector<BlockTxFeeInfo> &result)
{
  if (fromHeight > toHeight)
    return false;

  // Build batch JSON-RPC: array of getblockstats calls
  xmstream postData;
  {
    JSON::Array batch(postData);
    for (int64_t h = fromHeight; h <= toHeight; h++) {
      batch.addField();
      {
        JSON::Object query(postData);
        query.addString("method", "getblockstats");
        query.addField("params");
        {
          JSON::Array params(postData);
          params.addInt(h);
          params.addField();
          {
            JSON::Array fields(postData);
            fields.addString("totalfee");
            fields.addString("time");
          }
        }
      }
    }
  }

  rapidjson::Document document;
  if (ioRpcQuery(base, buildPostQuery(postData), document, 120 * 1000000) != EStatusOk) {
    CLOG_F(WARNING,
           "{} {}: getblockstats batch request failed",
           CoinInfo_.Name,
           FullHostName_);
    return false;
  }

  if (!document.IsArray()) {
    CLOG_F(WARNING,
           "{} {}: getblockstats batch: expected array response",
           CoinInfo_.Name,
           FullHostName_);
    return false;
  }

  int64_t expectedSize = toHeight - fromHeight + 1;
  if (static_cast<int64_t>(document.GetArray().Size()) != expectedSize) {
    CLOG_F(WARNING,
           "{} {}: getblockstats batch: expected {} results, got {}",
           CoinInfo_.Name,
           FullHostName_,
           expectedSize,
           document.GetArray().Size());
    return false;
  }

  result.reserve(result.size() + static_cast<size_t>(expectedSize));
  for (rapidjson::SizeType i = 0, ie = document.GetArray().Size(); i != ie; ++i) {
    rapidjson::Value &entry = document.GetArray()[i];
    if (!entry.HasMember("result") || !entry["result"].IsObject()) {
      CLOG_F(WARNING,
             "{} {}: getblockstats: invalid result at index {}",
             CoinInfo_.Name,
             FullHostName_,
             i);
      return false;
    }

    rapidjson::Value &r = entry["result"];
    if (!r.HasMember("totalfee") || !r["totalfee"].IsInt64() ||
        !r.HasMember("time") || !r["time"].IsInt64()) {
      CLOG_F(WARNING,
             "{} {}: getblockstats: missing fields at index {}",
             CoinInfo_.Name,
             FullHostName_,
             i);
      return false;
    }

    BlockTxFeeInfo info;
    info.Height = fromHeight + static_cast<int64_t>(i);
    info.Time = r["time"].GetInt64();
    info.TotalFee = r["totalfee"].GetInt64();
    result.push_back(info);
  }

  return true;
}

void CBitcoinRpcClient::poll()
{
  WorkFetcher_.LongPollId = HasLongPoll_ ? "0000000000000000000000000000000000000000000000000000000000000000" : "";
  WorkFetcher_.WorkId = 0;
  ++WorkFetchGeneration_;
  sendWorkFetchRequest();
}

void CBitcoinRpcClient::sendWorkFetchRequest()
{
  std::string rawRequest;
  uint64_t timeout;

  if (WorkFetcher_.LongPollId.empty()) {
    // Non-longpoll: use cached request, 60s timeout
    rawRequest = GbtRequestNoLongPoll_;
    timeout = 60000000;
  } else {
    // Longpoll: body changes (longPollId), no timeout
    std::string gbtBody = buildGetBlockTemplate(WorkFetcher_.LongPollId, CoinInfo_.SegwitEnabled, CoinInfo_.MWebEnabled);
    HttpRequest req;
    req.Method = HttpMethod::POST;
    req.Body = gbtBody;
    rawRequest = WorkFetcherHttpClient_.prepare(req);
    timeout = 0;
  }

  unsigned gen = WorkFetchGeneration_;
  WorkFetcherHttpClient_.aioRequest(std::move(rawRequest), [this, gen](AsyncOpStatus status, HttpResponse response) {
    if (gen != WorkFetchGeneration_)
      return;
    onWorkFetcherResponse(status, std::move(response));
  }, timeout);
}

void CBitcoinRpcClient::onWorkFetcherResponse(AsyncOpStatus status, HttpResponse response)
{
  if (status != aosSuccess || response.StatusCode != 200) {
    CLOG_FC(*LogChannel_, WARNING, "{} {}: request error code: {} (http result code: {}, data: {})",
            CoinInfo_.Name,
            FullHostName_,
            static_cast<unsigned>(status),
            response.StatusCode,
            response.Body.empty() ? "<null>" : response.Body.c_str());
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  std::unique_ptr<CBlockTemplate> blockTemplate(new CBlockTemplate(CoinInfo_.Name, CoinInfo_.WorkType));
  blockTemplate->Document.Parse(response.Body.data(), response.Body.size());
  if (blockTemplate->Document.HasParseError()) {
    CLOG_FC(*LogChannel_, WARNING, "{} {}: JSON parse error", CoinInfo_.Name, FullHostName_);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  if (!blockTemplate->Document["result"].IsObject()) {
    CLOG_FC(*LogChannel_, WARNING, "{} {}: JSON invalid format: no result object", CoinInfo_.Name, FullHostName_);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  int64_t height = 0;
  std::string prevBlockHash;
  std::string bits;
  bool validAcc = true;
  rapidjson::Value &resultObject = blockTemplate->Document["result"];
  jsonParseString(resultObject, "previousblockhash", prevBlockHash, true, &validAcc);
  jsonParseInt(resultObject, "height", &height, &validAcc);
  jsonParseString(resultObject, "bits", bits, true, &validAcc);
  if (!validAcc || prevBlockHash.size() < 16) {
    CLOG_FC(*LogChannel_, WARNING, "{} {}: getblocktemplate invalid format", CoinInfo_.Name, FullHostName_);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  if (!WorkFetcher_.LongPollId.empty()) {
    jsonParseString(resultObject, "longpollid", WorkFetcher_.LongPollId, true, &validAcc);
    if (!validAcc) {
      CLOG_FC(*LogChannel_, WARNING, "{} {}: does not support long poll, strongly recommended update your node", CoinInfo_.Name, FullHostName_);
      WorkFetcher_.LongPollId.clear();
    }
  }

  blockTemplate->Height = height;

  // Get unique work id
  uint64_t workId = blockTemplate->UniqueWorkId = readHexBE<uint64_t>(prevBlockHash.c_str(), 16);
  uint32_t nBits = strtoul(bits.c_str(), nullptr, 16);
  double difficulty;
  if (CoinInfo_.PowerUnitType == CCoinInfo::ECPD) {
    // PrimePOW: difficulty is chain length encoded as fixed-point (24-bit fractional part)
    difficulty = static_cast<double>(nBits) / static_cast<double>(1 << 24);
  } else {
    difficulty = UInt<256>::fpdiv(CoinInfo_.PowLimit, uint256Compact(nBits));
  }

  blockTemplate->Difficulty = difficulty;

  // Check new work available
  if (WorkFetcher_.WorkId != workId) {
    CLOG_FC(*LogChannel_, INFO, "{}: new work available; previous block: {}; height: {}; difficulty: {}", CoinInfo_.Name, prevBlockHash, static_cast<unsigned>(height), formatDifficulty(difficulty));
    Dispatcher_->onWorkFetcherNewWork(blockTemplate.release());
  }

  WorkFetcher_.WorkId = workId;

  // Send next request
  if (!WorkFetcher_.LongPollId.empty()) {
    sendWorkFetchRequest();
  } else {
    // Wait 1 second
    userEventStartTimer(WorkFetcher_.TimerEvent, 1*1000000, 1);
  }
}

void CBitcoinRpcClient::onWorkFetchTimeout()
{
  sendWorkFetchRequest();
}
