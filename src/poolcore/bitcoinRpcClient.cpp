#include "poolcore/bitcoinRPCClient.h"

#include "blockmaker/bitcoinBlockTemplate.h"
#include "poolcore/clientDispatcher.h"
#include "poolcommon/utils.h"
#include "asyncio/asyncio.h"
#include "loguru.hpp"

static std::string buildGetBlockTemplate(const std::string &longPollId, bool segwitEnabled, bool mwebEnabled)
{
  CGetBlockTemplateRequest request;
  request.Params.Opts.Capabilities = {"coinbasetxn", "workid", "coinbase/append"};
  if (!longPollId.empty())
    request.Params.Opts.Longpollid = longPollId;
  if (segwitEnabled)
    request.Params.Opts.Rules.push_back("segwit");
  if (mwebEnabled)
    request.Params.Opts.Rules.push_back("mweb");
  std::string result;
  request.serialize(result);
  return result;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::signRawTransaction(asyncBase *base, const std::string &fundedTransaction, std::string &signedTransaction, std::string &error)
{
  CSignRawTransactionResponse response;
  RpcQueryResult rpcResult;

  if (HasSignRawTransactionWithWallet_) {
    auto status = ioRpcQuery(base, buildPostQuery(CSignRawTransactionWithWalletRequest{.Params = {fundedTransaction, {}}}), response, 180000000, rpcResult);
    if (status != EStatusOk) {
      constexpr int RPC_METHOD_NOT_FOUND = -32601;
      if (rpcResult.ErrorCode == RPC_METHOD_NOT_FOUND) {
        HasSignRawTransactionWithWallet_ = false;
      } else {
        error = rpcResult.Error;
        return status;
      }
    }
  }

  if (!HasSignRawTransactionWithWallet_) {
    auto status = ioRpcQuery(base, buildPostQuery(CSignRawTransactionRequest{.Params = {fundedTransaction, {}}}), response, 180000000, rpcResult);
    if (status != EStatusOk) {
      error = rpcResult.Error;
      return status;
    }
  }

  if (!response.Result.has_value())
    return EStatusProtocolError;

  signedTransaction = std::move(response.Result->Hex);
  if (!response.Result->Complete) {
    if (response.Result->Error.has_value())
      error = std::move(*response.Result->Error);
    return EStatusUnknownError;
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

  GetWalletInfoRequest_ = buildPostQuery(CGetWalletInfoRequest{});
  BalanceRequest_ = buildPostQuery(CGetBalanceRequest{});
  BalanceWithImmaturedRequest_ = buildPostQuery(CGetBalanceRequest{.Params = {"*", 1}});
  BlockChainInfoRequest_ = buildPostQuery(CGetBlockChainInfoRequest{});
  InfoRequest_ = buildPostQuery(CGetInfoRequest{});

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


bool CBitcoinRpcClient::ioGetBalance(asyncBase *base, CNetworkClient::GetBalanceResult &result)
{
  if (HasGetWalletInfo_) {
    CGetWalletInfoResponse response;
    RpcQueryResult rpcResult;
    if (ioRpcQuery(base, GetWalletInfoRequest_, response, 10000000, rpcResult) == EStatusOk) {
      if (!response.Result.has_value()) {
        CLOG_F(WARNING, "{} {}: getwalletinfo invalid format", CoinInfo_.Name, FullHostName_);
        return false;
      }
      result.Balance = response.Result->Balance;
      result.Immatured = response.Result->Immature_balance;
      return true;
    } else if (rpcResult.HttpStatus == 404) {
      CLOG_F(WARNING, "{} {}: doesn't support getwalletinfo api; recommended update your node", CoinInfo_.Name, FullHostName_);
      HasGetWalletInfo_ = false;
    } else {
      return false;
    }
  }

  if (!HasGetWalletInfo_) {
    CGetBalanceResponse balanceResponse, fullBalanceResponse;
    if (ioRpcQuery(base, BalanceRequest_, balanceResponse, 10000000) == EStatusOk &&
        ioRpcQuery(base, BalanceWithImmaturedRequest_, fullBalanceResponse, 10000000) == EStatusOk) {
      if (!balanceResponse.Result.has_value() || !fullBalanceResponse.Result.has_value()) {
        CLOG_F(WARNING, "{} {}: getbalance invalid format", CoinInfo_.Name, FullHostName_);
        return false;
      }
      result.Balance = *balanceResponse.Result;
      result.Immatured = *fullBalanceResponse.Result - result.Balance;
      return true;
    }
    CLOG_F(INFO, "{} {}: ioGetBalance: getbalance failed", CoinInfo_.Name, FullHostName_);
  }

  return false;
}

bool CBitcoinRpcClient::ioGetBlockConfirmations(asyncBase *base, int64_t, std::vector<GetBlockConfirmationsQuery> &query)
{
  for (auto &it: query)
    it.Confirmations = -2;

  // Step 1: Get best block height
  uint64_t bestBlockHeight = 0;
  {
    CGetChainInfoResponse response;
    RpcQueryResult rpcResult;

    if (HasGetBlockChainInfo_) {
      auto status = ioRpcQuery(base, BlockChainInfoRequest_, response, 5000000, rpcResult);
      if (status == EStatusOk && response.Result.has_value()) {
        bestBlockHeight = response.Result->Blocks;
      } else if (status == EStatusOk || status == EStatusProtocolError) {
        CLOG_F(WARNING, "{} {}: doesn't support getblockchaininfo; recommended update your node", CoinInfo_.Name, FullHostName_);
        HasGetBlockChainInfo_ = false;
      } else {
        return false;
      }
    }

    if (!HasGetBlockChainInfo_) {
      CGetChainInfoResponse infoResponse;
      if (ioRpcQuery(base, InfoRequest_, infoResponse, 5000000) != EStatusOk || !infoResponse.Result.has_value()) {
        CLOG_F(WARNING, "{} {}: both getblockchaininfo and getinfo failed", CoinInfo_.Name, FullHostName_);
        return false;
      }
      bestBlockHeight = infoResponse.Result->Blocks;
    }
  }

  if (query.empty())
    return true;

  // Step 2: Batch getblockhash
  std::vector<CGetBlockHashRequest> requests(query.size());
  for (size_t i = 0; i < query.size(); i++)
    requests[i].Params = {query[i].Height};

  std::string batchBody;
  CGetBlockHashBatch{.Items = std::move(requests)}.serialize(batchBody);

  CRpcStringBatch batchResponse;
  if (ioRpcQuery(base, buildPostQuery(batchBody), batchResponse, 5000000) != EStatusOk)
    return false;

  if (batchResponse.Items.size() != query.size()) {
    CLOG_F(WARNING, "{} {}: getblockhash batch response size mismatch", CoinInfo_.Name, FullHostName_);
    return false;
  }

  for (size_t i = 0; i < query.size(); i++) {
    if (batchResponse.Items[i].Result.has_value()) {
      query[i].Confirmations = query[i].Hash == *batchResponse.Items[i].Result
        ? static_cast<int64_t>(bestBlockHeight - query[i].Height) : -1;
    } else {
      query[i].Confirmations = -1;
    }
  }

  return true;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const UInt<384> &value, BuildTransactionResult &result)
{
  std::string rawTransaction;
  std::string fundedTransaction;

  result.Value = value;
  while (result.Value.nonZero()) {
    // createrawtransaction
    {
      CCreateRawTransactionRequest req;
      req.Params.Outputs[address] = FormatMoney(result.Value, CoinInfo_.FractionalPartSize);

      CRpcStringResponse response;
      RpcQueryResult rpcResult;
      auto status = ioRpcQuery(base, buildPostQuery(req), response, 180000000, rpcResult);
      if (status != EStatusOk) {
        result.Error = rpcResult.Error;
        return status;
      }
      if (!response.Result.has_value())
        return EStatusProtocolError;
      rawTransaction = std::move(*response.Result);
    }

    // fundrawtransaction
    {
      CFundRawTransactionRequest req{.Params = {rawTransaction, {}}};
      if (CoinInfo_.HasExtendedFundRawTransaction)
        req.Params.Options = CFundRawTxOptions{changeAddress};

      CFundRawTransactionResponse response;
      RpcQueryResult rpcResult;
      auto status = ioRpcQuery(base, buildPostQuery(req), response, 180000000, rpcResult);
      if (status != EStatusOk) {
        constexpr int RPC_WALLET_INSUFFICIENT_FUNDS = -6;
        result.Error = rpcResult.Error;
        return rpcResult.ErrorCode == RPC_WALLET_INSUFFICIENT_FUNDS ? EStatusInsufficientFunds : status;
      }
      if (!response.Result.has_value())
        return EStatusProtocolError;

      fundedTransaction = std::move(response.Result->Hex);
      result.Fee = response.Result->Fee;
    }

    if (result.Value + result.Fee > value) {
      if (result.Value <= result.Fee) {
        result.Error = "too big fee";
        return EStatusUnknownError;
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

  // decoderawtransaction
  {
    CDecodeRawTransactionResponse response;
    RpcQueryResult rpcResult;
    auto status = ioRpcQuery(base, buildPostQuery(CDecodeRawTransactionRequest{.Params = {result.TxData}}), response, 180000000, rpcResult);
    if (status != EStatusOk) {
      result.Error = rpcResult.Error;
      return status;
    }
    if (!response.Result.has_value())
      return EStatusProtocolError;

    result.TxId = std::move(response.Result->Txid);
  }

  return EStatusOk;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioSendTransaction(asyncBase *base, const std::string &txData, const std::string&, std::string &error)
{
  CRpcStringResponse response;
  RpcQueryResult rpcResult;
  auto status = ioRpcQuery(base, buildPostQuery(CSendRawTransactionRequest{.Params = {txData}}), response, 180000000, rpcResult);
  if (status != EStatusOk) {
    constexpr int RPC_VERIFY_ERROR = -25;
    constexpr int RPC_VERIFY_REJECTED = -26;
    constexpr int RPC_VERIFY_ALREADY_IN_CHAIN = -27;
    error = rpcResult.Error;
    if (rpcResult.ErrorCode == RPC_VERIFY_ERROR && rpcResult.Error == "Missing inputs")
      return EStatusVerifyRejected;
    if (rpcResult.ErrorCode == RPC_VERIFY_REJECTED)
      return EStatusVerifyRejected;
    if (rpcResult.ErrorCode == RPC_VERIFY_ALREADY_IN_CHAIN)
      return EStatusOk;
    return status;
  }

  return EStatusOk;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, UInt<384> *txFee, std::string &error)
{
  *txFee = 0u;

  CGetTransactionResponse response;
  RpcQueryResult rpcResult;
  auto status = ioRpcQuery(base, buildPostQuery(CGetTransactionRequest{.Params = {txId}}), response, 180000000, rpcResult);
  if (status != EStatusOk) {
    constexpr int RPC_INVALID_ADDRESS_OR_KEY = -5;
    error = rpcResult.Error;
    return rpcResult.ErrorCode == RPC_INVALID_ADDRESS_OR_KEY ? EStatusInvalidAddressOrKey : status;
  }

  if (!response.Result.has_value())
    return EStatusProtocolError;

  *confirmations = response.Result->Confirmations;
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
        CRpcStringResponse parsed;
        if (parsed.parse(response.Body.data(), response.Body.size())) {
          if (!parsed.Result.has_value())
            success = true;
          else
            error = std::move(*parsed.Result);
        }
        if (response.StatusCode != 200 && error.empty() && parsed.Error.has_value())
          error = std::move(parsed.Error->Message);
      }

      operation->accept(success, FullHostName_, error);
    }, 180000000);
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioListUnspent(asyncBase *base, ListUnspentResult &result)
{
  CListUnspentResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(CListUnspentRequest{}), response, 180000000);
  if (status != EStatusOk)
    return status;
  if (!response.Result.has_value())
    return EStatusProtocolError;

  for (auto &out : *response.Result) {
    ListUnspentElement &element = result.Outs.emplace_back();
    element.Address = std::move(out.Address);
    element.Amount = out.Amount;
    element.IsCoinbase = out.Generated;
  }

  return EStatusOk;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioZSendMany(asyncBase *base, const std::string &source, const std::string &destination, const UInt<384> &amount, const std::string &memo, uint64_t minConf, const UInt<384> &fee, CNetworkClient::ZSendMoneyResult &result)
{
  CZSendManyDest dest;
  dest.Address = destination;
  dest.Amount = FormatMoney(amount, CoinInfo_.FractionalPartSize);
  if (!memo.empty())
    dest.Memo = memo;

  CZSendManyRequest req{.Params = {source, {std::move(dest)}, static_cast<int64_t>(minConf), FormatMoney(fee, CoinInfo_.FractionalPartSize)}};

  CRpcStringResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(req), response, 180000000);
  if (status != EStatusOk)
    return status;

  if (response.Result.has_value()) {
    result.AsyncOperationId = std::move(*response.Result);
    return EStatusOk;
  }

  if (response.Error.has_value())
    result.Error = std::move(response.Error->Message);
  return EStatusUnknownError;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioZGetBalance(asyncBase *base, const std::string &address, UInt<384> *balance)
{
  CGetBalanceResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(CZGetBalanceRequest{.Params = {address}}), response, 180000000);
  if (status != EStatusOk)
    return status;
  if (!response.Result.has_value())
    return EStatusProtocolError;

  *balance = *response.Result;
  return EStatusOk;
}

CNetworkClient::EOperationStatus CBitcoinRpcClient::ioWalletService(asyncBase*, std::string&)
{
  return CNetworkClient::EStatusOk;
}

bool CBitcoinRpcClient::ioGetBlockTxFees(asyncBase *base, int64_t fromHeight, int64_t toHeight, std::vector<BlockTxFeeInfo> &result)
{
  if (fromHeight > toHeight)
    return false;

  int64_t count = toHeight - fromHeight + 1;
  std::vector<CGetBlockStatsRequest> requests(static_cast<size_t>(count));
  for (int64_t i = 0; i < count; i++)
    requests[static_cast<size_t>(i)].Params = {fromHeight + i, {"totalfee", "time"}};

  std::string batchBody;
  CGetBlockStatsBatch{.Items = std::move(requests)}.serialize(batchBody);

  CGetBlockStatsResponseBatch response;
  if (ioRpcQuery(base, buildPostQuery(batchBody), response, 120000000) != EStatusOk) {
    CLOG_F(WARNING, "{} {}: getblockstats batch failed", CoinInfo_.Name, FullHostName_);
    return false;
  }

  if (static_cast<int64_t>(response.Items.size()) != count) {
    CLOG_F(WARNING, "{} {}: getblockstats batch: expected {} results, got {}", CoinInfo_.Name, FullHostName_, count, response.Items.size());
    return false;
  }

  result.reserve(result.size() + static_cast<size_t>(count));
  for (size_t i = 0; i < response.Items.size(); i++) {
    if (!response.Items[i].Result.has_value()) {
      CLOG_F(WARNING, "{} {}: getblockstats: invalid result at index {}", CoinInfo_.Name, FullHostName_, i);
      return false;
    }

    BlockTxFeeInfo info;
    info.Height = fromHeight + static_cast<int64_t>(i);
    info.Time = response.Items[i].Result->Time;
    info.TotalFee = response.Items[i].Result->Totalfee;
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
  if (status != aosSuccess) {
    CLOG_FC(*LogChannel_, WARNING, "{} {}: request error code: {} (http: {})",
            CoinInfo_.Name,
            FullHostName_,
            static_cast<unsigned>(status),
            response.StatusCode);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  std::unique_ptr<CBitcoinBlockTemplate> blockTemplate(new CBitcoinBlockTemplate(CoinInfo_.Name, CoinInfo_.WorkType));
  if (!blockTemplate->Data.parse(response.Body.data(), response.Body.size())) {
    CLOG_FC(*LogChannel_, WARNING, "{} {}: JSON parse error (http: {})", CoinInfo_.Name, FullHostName_, response.StatusCode);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  if (!blockTemplate->Data.Result.has_value()) {
    CLOG_FC(*LogChannel_, WARNING, "{} {}: JSON invalid format: no result object", CoinInfo_.Name, FullHostName_);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  CBlockTemplateResult &result = *blockTemplate->Data.Result;

  if (!WorkFetcher_.LongPollId.empty()) {
    if (result.Longpollid.has_value()) {
      WorkFetcher_.LongPollId = *result.Longpollid;
    } else {
      CLOG_FC(*LogChannel_, WARNING, "{} {}: does not support long poll, strongly recommended update your node", CoinInfo_.Name, FullHostName_);
      WorkFetcher_.LongPollId.clear();
    }
  }

  blockTemplate->Height = result.Height;

  // Get unique work id from low bytes of previousblockhash
  uint64_t workId;
  memcpy(&workId, result.Previousblockhash.begin(), sizeof(workId));
  blockTemplate->UniqueWorkId = workId;
  uint32_t nBits = strtoul(result.Bits.c_str(), nullptr, 16);
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
    CLOG_FC(*LogChannel_, INFO, "{}: new work available; previous block: {}; height: {}; difficulty: {}; transactions: {}", CoinInfo_.Name, result.Previousblockhash.getHexLE(), static_cast<unsigned>(result.Height), formatDifficulty(difficulty), result.Transactions.size());
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
