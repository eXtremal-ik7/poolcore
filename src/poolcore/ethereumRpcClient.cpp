#include "poolcore/ethereumRPCClient.h"
#include "ethereumRpc.idl.h"

#include "blockmaker/eth.h"
#include "blockmaker/ethash.h"
#include "poolcore/backend.h"
#include "blockmaker/ethereumBlockTemplate.h"
#include "poolcommon/utils.h"


CEthereumRpcClient::CEthereumRpcClient(asyncBase *base, const CCoinInfo &coinInfo, const char *address, const SelectorByWeight<CMiningAddress> &miningAddresses) :
  CoinInfo_(coinInfo),
  RpcEndpoint_([&]() -> std::string {
    std::string url = "http://";
    url += address;
    if (!strchr(address, ':')) {
      url.push_back(':');
      url.append(std::to_string(coinInfo.DefaultRpcPort));
    }
    return url;
  }().c_str()),
  WorkFetcherClient_(base, [&]() -> std::string {
    std::string url = "http://";
    url += address;
    if (!strchr(address, ':')) {
      url.push_back(':');
      url.append(std::to_string(coinInfo.DefaultRpcPort));
    }
    return url;
  }().c_str())
{
  if (!RpcEndpoint_.isValid()) {
    CLOG_F(ERROR, "{}: can't resolve address {}", coinInfo.Name, address);
    exit(1);
  }

  FullHostName_ = RpcEndpoint_.hostName();

  WorkFetcher_.TimerEvent = newUserEvent(base, 0, [](aioUserEvent*, void *arg) {
    static_cast<CEthereumRpcClient*>(arg)->onWorkFetchTimeout();
  }, this);
  WorkFetcher_.LastTemplateTime = std::chrono::time_point<std::chrono::steady_clock>::min();
  WorkFetcher_.WorkId = 0;
  WorkFetcher_.Height = 0;

  if (miningAddresses.size() != 1) {
    CLOG_F(ERROR, "ERROR: ethereum-based backends support working with only one mining address");
    exit(1);
  }

  MiningAddress_ = miningAddresses.getByIndex(0).MiningAddress;

  // Build EthGetWorkRequest_
  {
    std::string body;
    CEthGetWorkRequest{}.serialize(body);
    HttpRequest req;
    req.Method = HttpMethod::POST;
    req.Body = body;
    EthGetWorkRequest_ = WorkFetcherClient_.prepare(req);
  }
}

std::string CEthereumRpcClient::buildPostQuery(const char *data, size_t size)
{
  HttpRequest req;
  req.Method = HttpMethod::POST;
  req.Body = std::string_view(data, size);
  return RpcEndpoint_.prepare(req);
}

std::string CEthereumRpcClient::buildPostQuery(const std::string &jsonBody)
{
  return buildPostQuery(jsonBody.data(), jsonBody.size());
}


bool CEthereumRpcClient::ioGetBalance(asyncBase *base, CNetworkClient::GetBalanceResult &result)
{
  UInt<384> balance;
  if (ethGetBalance(base, MiningAddress_, &balance) != EStatusOk)
    return false;

  result.Balance = balance;
  result.Immatured = UInt<384>::zero();
  return true;
}

bool CEthereumRpcClient::ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<CNetworkClient::GetBlockConfirmationsQuery> &queries)
{
  for (auto &It: queries)
    It.Confirmations = -2;

  // First, get best chain height
  uint64_t bestBlockHeight = 0;
  if (ethBlockNumber(base, &bestBlockHeight) != EStatusOk)
    return false;

  for (auto &query: queries) {
    ETHBlock block;
    if (ethGetBlockByNumber(base, query.Height, block) != EStatusOk)
      return false;

    if (block.MixHash == UInt<256>::fromHex(query.Hash.c_str())) {
      query.Confirmations = bestBlockHeight - query.Height;
    } else {
      std::string publicHash;
      int64_t uncleHeight = ioSearchUncle(base, query.Height, query.Hash, bestBlockHeight, publicHash);
      if (uncleHeight) {
        query.Confirmations = bestBlockHeight - uncleHeight;
        // TODO: remove static_cast
      } else if (bestBlockHeight - query.Height < static_cast<uint64_t>(orphanAgeLimit)) {
        query.Confirmations = -3;
      } else {
        query.Confirmations = -1;
      }
    }
  }

  return true;
}

bool CEthereumRpcClient::ioGetBlockExtraInfo(asyncBase *base, int64_t orphanAgeLimit, std::vector<CNetworkClient::GetBlockExtraInfoQuery> &queries)
{
  for (auto &It: queries)
    It.Confirmations = -2;

  // First, get best chain height
  uint64_t bestBlockHeight = 0;
  if (ethBlockNumber(base, &bestBlockHeight) != EStatusOk)
    return false;

  for (auto &query: queries) {
    // Process each block separately
    ETHBlock block;
    if (ethGetBlockByNumber(base, query.Height, block) != EStatusOk)
      return false;

    if (block.MixHash != UInt<256>::fromHex(query.Hash.c_str())) {
      int64_t uncleHeight = ioSearchUncle(base, query.Height, query.Hash, bestBlockHeight, query.PublicHash);
      if (uncleHeight) {
        UInt<384> reward = ETH::getConstBlockReward(CoinInfo_.Name, uncleHeight) * (8 - (uncleHeight-query.Height)) / 8u;
        query.Confirmations = bestBlockHeight - uncleHeight;
        query.BlockReward = reward;
      // TODO: remove static_cast
      } else if (bestBlockHeight - query.Height < static_cast<uint64_t>(orphanAgeLimit)) {
        query.Confirmations = -3;
      } else {
        query.Confirmations = -1;
      }

      continue;
    } else {
      query.PublicHash = block.Hash.getHex();
    }

    // Get block reward
    UInt<384> constReward = ETH::getConstBlockReward(CoinInfo_.Name, query.Height);
    UInt<384> totalTxFee = query.TxFee;
    if (totalTxFee == 0u) {
      for (const auto &txObject: block.Transactions) {
        // Get receipt for each transaction
        ETHTransactionReceipt receipt;
        if (ethGetTransactionReceipt(base, txObject.Hash, receipt) != EStatusOk)
          return false;

        totalTxFee += txObject.GasPrice * receipt.GasUsed;
      }

      // TODO: use 128 bit integer everywhere for accounting
      // totalTxFee = fromGWei(gwei(totalTxFee));
    }

    UInt<384> unclesReward = (constReward / 32u) * static_cast<uint64_t>(block.Uncles.size());
    UInt<384> gasFee = block.GasUsed * block.BaseFeePerGas;
    UInt<384> blockReward = constReward + unclesReward + totalTxFee - gasFee;

    query.TxFee = totalTxFee;
    query.BlockReward = blockReward;
    query.Confirmations = bestBlockHeight - query.Height;
  }

  return true;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ioBuildTransaction(asyncBase *base, const std::string &address, const std::string&, const UInt<384> &value, CNetworkClient::BuildTransactionResult &result)
{
  EOperationStatus status;

  // Check balance first
  // We require payout value + 5% at balance
  UInt<384> balance;
  status = ethGetBalance(base, MiningAddress_, &balance);
  if (status != CNetworkClient::EStatusOk) {
    result.Error = LastRpcError_;
    return status;
  }
  if (balance < (value + value/20u))
    return CNetworkClient::EStatusInsufficientFunds;

  // We need those values:
  //   * base fee per gas (eth.maxPriorityFeePerGas - eth.gasPrice)
  //   * max priority fee per gas (eth.maxPriorityFeePerGas)
  //   * max fee per gas (eth.maxPriorityFeePerGas + 2*baseFeePerGas)
  //   * nonce (eth.getTransactionCount)
  UInt<384> gasPrice = 0u;
  UInt<384> baseFeePerGas = 0u;
  UInt<384> maxPriorityFeePerGas = 0u;
  UInt<384> maxFeePerGas = 0u;
  uint64_t nonce = 0;

  if ((status = ethGasPrice(base, &gasPrice)) != CNetworkClient::EStatusOk ||
      (status = ethMaxPriorityFeePerGas(base, &maxPriorityFeePerGas)) != CNetworkClient::EStatusOk ||
      (status = ethGetTransactionCount(base, MiningAddress_, &nonce)) != EStatusOk ||
      (status = personalUnlockAccount(base, MiningAddress_, "", 60)) != EStatusOk) {
    result.Error = LastRpcError_;
    return status;
  }

  baseFeePerGas = gasPrice - maxPriorityFeePerGas;
  maxFeePerGas = maxPriorityFeePerGas + 2u*baseFeePerGas;

  if (CoinInfo_.Name != "ETC") {
    status = ethSignTransaction1559(base,
                                    MiningAddress_,
                                    address,
                                    value,
                                    21000u,
                                    maxPriorityFeePerGas,
                                    maxFeePerGas,
                                    nonce,
                                    result.TxData,
                                    result.TxId);
  } else {
    status = ethSignTransactionOld(base,
                                   MiningAddress_,
                                   address,
                                   value,
                                   21000u,
                                   gasPrice,
                                   nonce,
                                   result.TxData,
                                   result.TxId);
  }

  if (status != EStatusOk) {
    result.Error = LastRpcError_;
    return status;
  }

  // Can't determine fee at this moment
  result.Value = value;
  result.Fee = UInt<384>::zero();
  return CNetworkClient::EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ioSendTransaction(asyncBase *base, const std::string &txData, const std::string &txId, std::string &error)
{
  CNetworkClient::EOperationStatus status;

  if ((status = ethSendRawTransaction(base, txData)) != EStatusOk) {
    constexpr int RPC_INVALID_INPUT = -32000;

    error = LastRpcError_;
    if (LastRpcErrorCode_ == RPC_INVALID_INPUT) {
      if (LastRpcError_ == "already known") {
        return CNetworkClient::EStatusOk;
      } else if (LastRpcError_ == "nonce too low") {
        ETHTransactionReceipt receipt;
        if ((status = ethGetTransactionReceipt(base, UInt<256>::fromHex(txId.c_str()), receipt)) != EStatusOk)
          return status;

        return CNetworkClient::EStatusOk;
      } else {
        return EStatusVerifyRejected;
      }
    }

    return status;
  }

  return CNetworkClient::EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, UInt<384> *txFee, std::string &error)
{
  CNetworkClient::EOperationStatus status;

  ETHTransaction tx;
  ETHTransactionReceipt receipt;

  UInt<256> ltxId = UInt<256>::fromHex(txId.c_str());
  if ((status = ethGetTransactionByHash(base, ltxId, tx)) != EStatusOk) {
    error = LastRpcError_;
    return status;
  }
  if ((status = ethGetTransactionReceipt(base, ltxId, receipt)) != EStatusOk) {
    error = LastRpcError_;
    return EStatusInvalidAddressOrKey;
  }

  // get pending block
  uint64_t bestBlockHeight;
  if ((status = ethBlockNumber(base, &bestBlockHeight)) != EStatusOk) {
    error = LastRpcError_;
    return status;
  }

  *confirmations = bestBlockHeight - receipt.BlockNumber;
  *txFee = receipt.GasUsed * tx.GasPrice;
  return CNetworkClient::EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ioWalletService(asyncBase*, std::string&)
{
  return CNetworkClient::EStatusOk;
}

void CEthereumRpcClient::aioSubmitBlock(asyncBase *base, const void *data, size_t, CNetworkClient::CSubmitBlockOperation *operation)
{
  const ETH::BlockSubmitData *blockSubmitData = reinterpret_cast<const ETH::BlockSubmitData*>(data);

  CEthSubmitWorkRequest request;
  request.Params.Nonce = blockSubmitData->Nonce;
  request.Params.HeaderHash = blockSubmitData->HeaderHash;
  request.Params.MixHash = blockSubmitData->MixHash;

  RpcEndpoint_.aioRequest(base, buildPostQuery(request),
    [this, operation](AsyncOpStatus status, HttpResponse response) {
      bool success = false;
      std::string error;
      if (status != aosSuccess) {
        error = "http request error";
      } else {
        CEthRpcBoolResponse parsed;
        if (parsed.parse(response.Body.data(), response.Body.size())) {
          if (parsed.Result.has_value() && *parsed.Result)
            success = true;
          else
            error = "?";
        }
        if (response.StatusCode != 200 && error.empty() && parsed.Error.has_value())
          error = std::move(parsed.Error->Message);
      }
      operation->accept(success, FullHostName_, error);
    }, 180000000);
}

void CEthereumRpcClient::poll()
{
  WorkFetcher_.LastTemplateTime = std::chrono::time_point<std::chrono::steady_clock>::min();
  WorkFetcher_.WorkId = 0;
  WorkFetcher_.Height = 0;

  WorkFetcherClient_.aioRequest(EthGetWorkRequest_,
    [this](AsyncOpStatus status, HttpResponse response) {
      onWorkFetcherResponse(status, std::move(response));
    }, 10000000);
}

void CEthereumRpcClient::onWorkFetcherResponse(AsyncOpStatus status, HttpResponse response)
{
  if (status != aosSuccess) {
    CLOG_FC(*LogChannel_, WARNING, "{} {}: request error code: {} (http: {})",
            CoinInfo_.Name,
            FullHostName_,
            static_cast<unsigned>(status),
            response.StatusCode);
    onConnectionLost();
    return;
  }

  auto blockTemplate = std::make_unique<CEthereumBlockTemplate>(CoinInfo_.Name, CoinInfo_.WorkType);
  if (!blockTemplate->Data.parse(response.Body.data(), response.Body.size()) || !blockTemplate->Data.Result.has_value()) {
    CLOG_FC(*LogChannel_, WARNING, "{} {}: JSON parse error (http: {})", CoinInfo_.Name, FullHostName_, response.StatusCode);
    onConnectionLost();
    return;
  }

  CETHGetWorkResult &work = *blockTemplate->Data.Result;

  BaseBlob<256> seedHash = work.Seed_hash;
  std::reverse(seedHash.begin(), seedHash.end());

  UInt<256> target;
  if (work.Target.size() >= 3 && work.Target[0] == '0' && work.Target[1] == 'x')
    target.setHex(work.Target.c_str() + 2);
  double difficulty = UInt<256>::fpdiv(CoinInfo_.PowLimit, target);

  uint64_t height = 0;
  if (work.Block_height.size() >= 3 && work.Block_height[0] == '0' && work.Block_height[1] == 'x')
    height = strtoul(work.Block_height.c_str() + 2, nullptr, 16);
  uint64_t workId = height;

  // Check DAG presence
  int epochNumber = ethashGetEpochNumber(seedHash.begin());
  if (epochNumber == -1) {
    CLOG_FC(*LogChannel_, ERROR, "Can't find epoch number for seed {}", seedHash.getHexLE());
    onConnectionLost();
    return;
  }

  // For ETC
  if (CoinInfo_.BigEpoch)
    epochNumber /= 2;

  blockTemplate->Height = static_cast<int64_t>(height);
  blockTemplate->Difficulty = difficulty;

  if (!onResolveDag(blockTemplate.get(), epochNumber)) {
    onConnectionLost();
    return;
  }

  if (WorkFetcher_.WorkId != workId) {
    CLOG_FC(*LogChannel_, INFO, "{}: new work available; height: {}; difficulty: {}", CoinInfo_.Name, height, formatDifficulty(difficulty));
    onNewWork(blockTemplate.release());
    WorkFetcher_.Height = height;
    WorkFetcher_.WorkId = workId;
  }

  // Wait 100ms
  userEventStartTimer(WorkFetcher_.TimerEvent, 1*100000, 1);
}

void CEthereumRpcClient::onWorkFetchTimeout()
{
  WorkFetcherClient_.aioRequest(EthGetWorkRequest_,
    [this](AsyncOpStatus status, HttpResponse response) {
      onWorkFetcherResponse(status, std::move(response));
    }, 10000000);
}

int64_t CEthereumRpcClient::ioSearchUncle(asyncBase *base, int64_t height, const std::string &mixHash, int64_t bestBlockHeight, std::string &publicHash)
{
  int64_t maxHeight = std::min(height + 16, bestBlockHeight);

  for (int64_t currentHeight = height; currentHeight <= maxHeight; currentHeight++) {
    ETHBlock currentBlock;
    if (ethGetBlockByNumber(base, currentHeight, currentBlock) != EStatusOk)
      return false;

    for (unsigned uncleIdx = 0; uncleIdx < currentBlock.Uncles.size(); uncleIdx++) {
      ETHBlock uncleBlock;
      if (ethGetUncleByBlockNumberAndIndex(base, currentHeight, uncleIdx, uncleBlock) != EStatusOk)
        return false;

      if (uncleBlock.MixHash == UInt<256>::fromHex(mixHash.c_str())) {
        publicHash = uncleBlock.Hash.getHex(true, false, false);
        return currentHeight;
      }
    }
  }

  return 0;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGetBalance(asyncBase *base, const std::string &address, UInt<384> *balance)
{
  CEthGetBalanceRequest request;
  request.Params.Address = address;

  CEthGetBalanceResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(request), response, 5*1000000);
  if (status != EStatusOk)
    return status;
  if (!response.Result.has_value())
    return EStatusProtocolError;

  *balance = *response.Result;
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGasPrice(asyncBase *base, UInt<384> *gasPrice)
{
  CEthGetBalanceResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(CEthGasPriceRequest{}), response, 5*1000000);
  if (status != EStatusOk)
    return status;
  if (!response.Result.has_value())
    return EStatusProtocolError;

  *gasPrice = *response.Result;
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethMaxPriorityFeePerGas(asyncBase *base, UInt<384> *maxPriorityFeePerGas)
{
  CEthGetBalanceResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(CEthMaxPriorityFeePerGasRequest{}), response, 5*1000000);
  if (status != EStatusOk)
    return status;
  if (!response.Result.has_value())
    return EStatusProtocolError;

  *maxPriorityFeePerGas = *response.Result;
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGetTransactionCount(asyncBase *base, const std::string &address, uint64_t *count)
{
  CEthGetTransactionCountRequest request;
  request.Params.Address = address;

  CEthUint64HexResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(request), response, 5*1000000);
  if (status != EStatusOk)
    return status;
  if (!response.Result.has_value())
    return EStatusProtocolError;

  *count = *response.Result;
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethBlockNumber(asyncBase *base, uint64_t *blockNumber)
{
  CEthUint64HexResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(CEthBlockNumberRequest{}), response, 5*1000000);
  if (status != EStatusOk)
    return status;
  if (!response.Result.has_value())
    return EStatusProtocolError;

  *blockNumber = *response.Result;
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGetBlockByNumber(asyncBase *base, uint64_t height, ETHBlock &block)
{
  CEthGetBlockByNumberRequest request;
  request.Params.BlockNumber = height;
  request.Params.FullTransactions = true;

  CEthGetBlockByNumberResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(request), response, 60*1000000);
  if (status != EStatusOk)
    return status;
  if (!response.Result.has_value()) {
    CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
    return EStatusProtocolError;
  }

  CEthBlockResult &result = *response.Result;
  block.Hash = result.Hash;
  block.MixHash = result.MixHash;
  block.GasUsed = result.GasUsed;
  if (result.BaseFeePerGas.has_value())
    block.BaseFeePerGas = *result.BaseFeePerGas;

  for (auto &txResult : result.Transactions) {
    ETHTransaction &tx = block.Transactions.emplace_back();
    tx.GasPrice = txResult.GasPrice;
    tx.Hash = txResult.Hash;
  }

  block.Uncles = std::move(result.Uncles);
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGetUncleByBlockNumberAndIndex(asyncBase *base, uint64_t height, unsigned uncleIndex, ETHBlock &block)
{
  CEthGetUncleByBlockNumberAndIndexRequest request;
  request.Params.BlockNumber = height;
  request.Params.UncleIndex = uncleIndex;

  CEthGetUncleResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(request), response, 5*1000000);
  if (status != EStatusOk)
    return status;
  if (!response.Result.has_value()) {
    CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
    return EStatusProtocolError;
  }

  CEthUncleResult &result = *response.Result;
  block.Hash = result.Hash;
  block.MixHash = result.MixHash;
  block.GasUsed = result.GasUsed;
  if (result.BaseFeePerGas.has_value())
    block.BaseFeePerGas = *result.BaseFeePerGas;

  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGetTransactionByHash(asyncBase *base, const UInt<256> &txid, ETHTransaction &tx)
{
  CEthGetTransactionByHashRequest request;
  request.Params.TxHash = txid.getHex(true, true, false);

  CEthGetTransactionByHashResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(request), response, 5*1000000);
  if (status != EStatusOk)
    return status;
  if (!response.Result.has_value())
    return EStatusInvalidAddressOrKey;

  tx.GasPrice = response.Result->GasPrice;
  tx.Hash = txid;
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGetTransactionReceipt(asyncBase *base, const UInt<256> &txid, ETHTransactionReceipt &receipt)
{
  CEthGetTransactionReceiptRequest request;
  request.Params.TxHash = txid.getHex(true, true, false);

  CEthGetTransactionReceiptResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(request), response, 5*1000000);
  if (status != EStatusOk)
    return status;
  if (!response.Result.has_value()) {
    CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
    return EStatusProtocolError;
  }

  receipt.GasUsed = response.Result->GasUsed;
  receipt.BlockNumber = response.Result->BlockNumber;
  return EStatusOk;
}

static void stripHexPrefix(std::string &s)
{
  if (s.size() >= 2 && s[0] == '0' && s[1] == 'x')
    s.erase(0, 2);
}

static CNetworkClient::EOperationStatus parseSignTransactionResponse(CEthSignTransactionResponse &response, std::string &txData, std::string &txId)
{
  if (!response.Result.has_value())
    return CNetworkClient::EStatusProtocolError;
  txData = std::move(response.Result->Raw);
  stripHexPrefix(txData);
  txId = std::move(response.Result->Tx.Hash);
  stripHexPrefix(txId);
  return CNetworkClient::EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethSignTransactionOld(asyncBase *base,
                                                                           const std::string &from,
                                                                           const std::string &to,
                                                                           const UInt<384> &value,
                                                                           uint64_t gas,
                                                                           const UInt<384> &gasPrice,
                                                                           uint64_t nonce,
                                                                           std::string &txData,
                                                                           std::string &txId)
{
  CEthSignTransactionOldRequest request;
  request.Params.Tx.From = from;
  request.Params.Tx.To = to;
  request.Params.Tx.Value = value;
  request.Params.Tx.Gas = gas;
  request.Params.Tx.GasPrice = gasPrice;
  request.Params.Tx.Nonce = nonce;

  CEthSignTransactionResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(request), response, 5*1000000);
  if (status != EStatusOk)
    return status;

  return parseSignTransactionResponse(response, txData, txId);
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethSignTransaction1559(asyncBase *base,
                                                                            const std::string &from,
                                                                            const std::string &to,
                                                                            const UInt<384> &value,
                                                                            uint64_t gas,
                                                                            const UInt<384> &maxPriorityFeePerGas,
                                                                            const UInt<384> &maxFeePerGas,
                                                                            uint64_t nonce,
                                                                            std::string &txData,
                                                                            std::string &txId)
{
  CEthSignTransaction1559Request request;
  request.Params.Tx.From = from;
  request.Params.Tx.To = to;
  request.Params.Tx.Value = value;
  request.Params.Tx.Gas = gas;
  request.Params.Tx.MaxPriorityFeePerGas = maxPriorityFeePerGas;
  request.Params.Tx.MaxFeePerGas = maxFeePerGas;
  request.Params.Tx.Nonce = nonce;

  CEthSignTransactionResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(request), response, 5*1000000);
  if (status != EStatusOk)
    return status;

  return parseSignTransactionResponse(response, txData, txId);
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethSendRawTransaction(asyncBase *base, const std::string &txData)
{
  CEthSendRawTransactionRequest request;
  request.Params.TxData = "0x" + txData;

  CEthRpcHexResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(request), response, 5*1000000);
  if (status != EStatusOk)
    return status;
  if (!response.Result.has_value())
    return EStatusProtocolError;

  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::personalUnlockAccount(asyncBase *base,
                                                                           const std::string &address,
                                                                           const std::string &passPhrase,
                                                                           unsigned seconds)
{
  CPersonalUnlockAccountRequest request;
  request.Params.Address = address;
  request.Params.PassPhrase = passPhrase;
  request.Params.Seconds = seconds;

  CEthRpcBoolResponse response;
  auto status = ioRpcQuery(base, buildPostQuery(request), response, 180*1000000);
  if (status != EStatusOk)
    return status;
  if (!response.Result.has_value())
    return EStatusProtocolError;

  if (!*response.Result) {
    LastRpcError_ = "Can't unlock wallet";
    return EStatusUnknownError;
  }

  return EStatusOk;
}
