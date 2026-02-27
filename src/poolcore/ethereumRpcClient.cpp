#include "poolcore/ethereumRPCClient.h"

#include "blockmaker/eth.h"
#include "blockmaker/ethash.h"
#include "poolcore/backend.h"
#include "poolcore/blockTemplate.h"
#include "poolcore/clientDispatcher.h"
#include "poolcommon/jsonSerializer.h"
#include "asyncio/asyncio.h"
#include "p2putils/uriParse.h"

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

// Parse hex string (with or without "0x" prefix) as wei and return UInt<384> with wei in upper 128 bits
static UInt<384> fromWeiHex(const char *hex) {
  if (hex[0] == '0' && hex[1] == 'x')
    hex += 2;
  return UInt<384>::fromHex(hex) << 256;
}

// Convert UInt<384> (wei in upper 128 bits) to hex string with "0x" prefix
static std::string toWeiHex(const UInt<384> &value) {
  return (value >> 256).getHex(false, true, false);
}

static std::string buildPostQuery(const std::string address, const char *data, size_t size, const std::string &host)
{
  char dataLength[16];
  xitoa(size, dataLength);

  std::string query = "POST ";
    query.append(address);
    query.append(" HTTP/1.1\r\n");
    query.append("Host: ");
      query.append(host);
      query.append("\r\n");
    query.append("Connection: keep-alive\r\n");
    query.append("Content-Length: ");
      query.append(static_cast<const char*>(dataLength));
      query.append("\r\n");
    query.append("Content-Type: application/json\r\n");
    query.append("\r\n");
  if (data)
    query.append(data, size);
  return query;
}

static void buildPostQuery(const std::string address, const char *data, size_t size, const std::string &host, xmstream &out)
{
  char dataLength[16];
  xitoa(size, dataLength);

  out.write("POST ");
    out.write(address.data(), address.size());
    out.write(" HTTP/1.1\r\n");
    out.write("Host: ");
      out.write(host.data(), host.size());
      out.write("\r\n");
    out.write("Connection: keep-alive\r\n");
    out.write("Content-Length: ");
      out.write(static_cast<const char*>(dataLength));
      out.write("\r\n");
    out.write("Content-Type: application/json\r\n");
    out.write("\r\n");
  if (data)
    out.write(data, size);
}

CEthereumRpcClient::CEthereumRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, PoolBackendConfig &config) : CNetworkClient(threadsNum),
  WorkFetcherBase_(base), CoinInfo_(coinInfo)
{
  WorkFetcher_.Client = nullptr;
  httpParseDefaultInit(&WorkFetcher_.ParseCtx);
  WorkFetcher_.TimerEvent = newUserEvent(base, 0, [](aioUserEvent*, void *arg) {
    static_cast<CEthereumRpcClient*>(arg)->onWorkFetchTimeout();
  }, this);

  URI uri;
  std::string uriAddress = (std::string)"http://" + address;
  if (!uriParse(uriAddress.c_str(), &uri)) {
    CLOG_F(ERROR, "{}: can't parse address {}", coinInfo.Name, address);
    exit(1);
  }

  uint16_t port = uri.port ? uri.port : coinInfo.DefaultRpcPort;
  if (!uri.domain.empty()) {
    struct hostent *host = gethostbyname(uri.domain.c_str());
    if (host) {
      struct in_addr **hostAddrList = (struct in_addr**)host->h_addr_list;
      if (hostAddrList[0]) {
        Address_.ipv4 = hostAddrList[0]->s_addr;
        Address_.port = htons(port);
        Address_.family = AF_INET;
      } else {
        CLOG_F(ERROR, "{}: can't lookup address {}", coinInfo.Name, uri.domain);
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

  if (config.MiningAddresses.size() != 1) {
    CLOG_F(ERROR, "ERROR: ethereum-based backends support working with only one mining address");
    exit(1);
  }

  MiningAddress_ = config.MiningAddresses.getByIndex(0).MiningAddress;

  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  // getWork
  {
    {
      jsonStream.reset();
      JSON::Object queryObject(jsonStream);
      queryObject.addString("method", "eth_getWork");
      queryObject.addField("params");
      {
        JSON::Array paramsArray(jsonStream);
      }
      queryObject.addInt("id", -1);
    }

    buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_, EthGetWork_);
  }
}

CPreparedQuery *CEthereumRpcClient::prepareBlock(const void *data, size_t)
{
  const ETH::BlockSubmitData *blockSubmitData = reinterpret_cast<const ETH::BlockSubmitData*>(data);
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));
  jsonStream.reset();
  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_submitWork");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
      paramsArray.addString(blockSubmitData->Nonce);
      paramsArray.addString(blockSubmitData->HeaderHash);
      paramsArray.addString(blockSubmitData->MixHash);
    }
    queryObject.addInt("id", -1);
  }

  CPreparedSubmitBlock *query = new CPreparedSubmitBlock(this);
  query->stream().reset();
  buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_, query->stream());
  return query;
}

bool CEthereumRpcClient::ioGetBalance(asyncBase *base, GetBalanceResult &result)
{
  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return false;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, ConnectTimeout) != 0)
    return false;

  UInt<384> balance;
  if (ethGetBalance(connection.get(), MiningAddress_, &balance) != EStatusOk)
    return false;

  result.Balance = balance;
  result.Immatured = UInt<384>::zero();
  return true;
}

bool CEthereumRpcClient::ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockConfirmationsQuery> &queries)
{
  for (auto &It: queries)
    It.Confirmations = -2;

  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return false;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, ConnectTimeout) != 0)
    return false;

  // First, get best chain height
  uint64_t bestBlockHeight = 0;
  if (ethBlockNumber(connection.get(), &bestBlockHeight) != EStatusOk)
    return false;

  for (auto &query: queries) {
    ETHBlock block;
    if (ethGetBlockByNumber(connection.get(), query.Height, block) != EStatusOk)
      return false;

    if (block.MixHash == UInt<256>::fromHex(query.Hash.c_str())) {
      query.Confirmations = bestBlockHeight - query.Height;
    } else {
      std::string publicHash;
      int64_t uncleHeight = ioSearchUncle(connection.get(), query.Height, query.Hash, bestBlockHeight, publicHash);
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

bool CEthereumRpcClient::ioGetBlockExtraInfo(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockExtraInfoQuery> &queries)
{
  for (auto &It: queries)
    It.Confirmations = -2;

  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return false;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, ConnectTimeout) != 0)
    return false;

  // First, get best chain height
  uint64_t bestBlockHeight = 0;
  if (ethBlockNumber(connection.get(), &bestBlockHeight) != EStatusOk)
    return false;

  for (auto &query: queries) {
    // Process each block separately
    ETHBlock block;
    if (ethGetBlockByNumber(connection.get(), query.Height, block) != EStatusOk)
      return false;

    if (block.MixHash != UInt<256>::fromHex(query.Hash.c_str())) {
      int64_t uncleHeight = ioSearchUncle(connection.get(), query.Height, query.Hash, bestBlockHeight, query.PublicHash);
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
        if (ethGetTransactionReceipt(connection.get(), txObject.Hash, receipt) != EStatusOk)
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

CNetworkClient::EOperationStatus CEthereumRpcClient::ioBuildTransaction(asyncBase *base, const std::string &address, const std::string&, const UInt<384> &value, BuildTransactionResult &result)
{
  EOperationStatus status;
  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return CNetworkClient::EStatusNetworkError;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, ConnectTimeout) != 0)
    return CNetworkClient::EStatusNetworkError;

  // Check balance first
  // We require payout value + 5% at balance
  UInt<384> balance;
  status = ethGetBalance(connection.get(), MiningAddress_, &balance);
  if (status != CNetworkClient::EStatusOk) {
    result.Error = connection->LastError;
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

  if ((status = ethGasPrice(connection.get(), &gasPrice)) != CNetworkClient::EStatusOk ||
      (status = ethMaxPriorityFeePerGas(connection.get(), &maxPriorityFeePerGas)) != CNetworkClient::EStatusOk ||
      (status = ethGetTransactionCount(connection.get(), MiningAddress_, &nonce)) != EStatusOk ||
      (status = personalUnlockAccount(connection.get(), MiningAddress_, "", 60)) != EStatusOk) {
    result.Error = connection->LastError;
    return status;
  }

  baseFeePerGas = gasPrice - maxPriorityFeePerGas;
  maxFeePerGas = maxPriorityFeePerGas + 2u*baseFeePerGas;

  if (CoinInfo_.Name != "ETC") {
    status = ethSignTransaction1559(connection.get(),
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
    status = ethSignTransactionOld(connection.get(),
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
    result.Error = connection->LastError;
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
  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return CNetworkClient::EStatusNetworkError;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, ConnectTimeout) != 0)
    return CNetworkClient::EStatusNetworkError;

  if ((status = ethSendRawTransaction(connection.get(), txData)) != EStatusOk) {
    constexpr int RPC_INVALID_INPUT = -32000;

    error = connection->LastError;
    if (connection->LastErrorCode == RPC_INVALID_INPUT) {
      if (connection->LastError == "already known") {
        return CNetworkClient::EStatusOk;
      } else if (connection->LastError == "nonce too low") {
        ETHTransactionReceipt receipt;
        if ((status = ethGetTransactionReceipt(connection.get(), UInt<256>::fromHex(txId.c_str()), receipt)) != EStatusOk)
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
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return CNetworkClient::EStatusNetworkError;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, ConnectTimeout) != 0)
    return CNetworkClient::EStatusNetworkError;

  ETHTransaction tx;
  ETHTransactionReceipt receipt;

  UInt<256> ltxId = UInt<256>::fromHex(txId.c_str());
  if ((status = ethGetTransactionByHash(connection.get(), ltxId, tx)) != EStatusOk) {
    error = connection->LastError;
    return status;
  }
  if ((status = ethGetTransactionReceipt(connection.get(), ltxId, receipt)) != EStatusOk) {
    error = connection->LastError;
    return EStatusInvalidAddressOrKey;
  }

  // get pending block
  uint64_t bestBlockHeight;
  if ((status = ethBlockNumber(connection.get(), &bestBlockHeight)) != EStatusOk) {
    error = connection->LastError;
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

void CEthereumRpcClient::aioSubmitBlock(asyncBase *base, CPreparedQuery *queryPtr, CSubmitBlockOperation *operation)
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
      query->Operation->accept(false, query->client<CEthereumRpcClient>()->FullHostName_, "http connection error");
      delete query;
      return;
    }

    aioHttpRequest(httpClient, query->stream().data<const char>(), query->stream().sizeOf(), 180000000, httpParseDefault, &query->Connection->ParseCtx, [](AsyncOpStatus status, HTTPClient *, void *arg) {
      CPreparedSubmitBlock *query = static_cast<CPreparedSubmitBlock*>(arg);
      if (status != aosSuccess) {
        query->Operation->accept(false, query->client<CEthereumRpcClient>()->FullHostName_, "http request error");
        delete query;
        return;
      }

      query->client<CEthereumRpcClient>()->submitBlockRequestCb(query);
    }, query);
  }, query);
}

void CEthereumRpcClient::poll()
{
  socketTy S = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  aioObject *object = newSocketIo(WorkFetcherBase_, S);
  WorkFetcher_.Client = httpClientNew(WorkFetcherBase_, object);
  WorkFetcher_.LastTemplateTime = std::chrono::time_point<std::chrono::steady_clock>::min();
  WorkFetcher_.WorkId = 0;
  WorkFetcher_.Height = 0;
  dynamicBufferClear(&WorkFetcher_.ParseCtx.buffer);

  aioHttpConnect(WorkFetcher_.Client, &Address_, nullptr, 3000000, [](AsyncOpStatus status, HTTPClient*, void *arg){
    static_cast<CEthereumRpcClient*>(arg)->onWorkFetcherConnect(status);
  }, this);
}

void CEthereumRpcClient::onWorkFetcherConnect(AsyncOpStatus status)
{
  if (status != aosSuccess) {
    // TODO: inform dispatcher
    httpClientDelete(WorkFetcher_.Client);
    Dispatcher_->onWorkFetcherConnectionError();
    return;
  }

  aioHttpRequest(WorkFetcher_.Client, EthGetWork_.data<const char>(), EthGetWork_.sizeOf(), 10000000, httpParseDefault, &WorkFetcher_.ParseCtx, [](AsyncOpStatus status, HTTPClient*, void *arg){
    static_cast<CEthereumRpcClient*>(arg)->onWorkFetcherIncomingData(status);
  }, this);
}

void CEthereumRpcClient::onWorkFetcherIncomingData(AsyncOpStatus status)
{
  if (status != aosSuccess || WorkFetcher_.ParseCtx.resultCode != 200) {
    CLOG_FC(*LogChannel_, WARNING, "{} {}: request error code: {} (http result code: {}, data: {})",
            CoinInfo_.Name,
            FullHostName_,
            static_cast<unsigned>(status),
            WorkFetcher_.ParseCtx.resultCode,
            WorkFetcher_.ParseCtx.body.data ? WorkFetcher_.ParseCtx.body.data : "<null>");
    httpClientDelete(WorkFetcher_.Client);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  std::unique_ptr<CBlockTemplate> blockTemplate(new CBlockTemplate(CoinInfo_.Name, CoinInfo_.WorkType));
  blockTemplate->Document.Parse(WorkFetcher_.ParseCtx.body.data);
  if (blockTemplate->Document.HasParseError()) {
    CLOG_FC(*LogChannel_, WARNING, "{} {}: JSON parse error", CoinInfo_.Name, FullHostName_);
    httpClientDelete(WorkFetcher_.Client);
    Dispatcher_->onWorkFetcherConnectionLost();
    return;
  }

  uint64_t height = 0;
  bool templateIsOk = false;
  uint64_t workId = 0;
  double difficulty = 0.0;

  for (;;) {
    if (!blockTemplate->Document.HasMember("result") || !blockTemplate->Document["result"].IsArray())
      break;
    rapidjson::Value::Array resultValue = blockTemplate->Document["result"].GetArray();
    if (resultValue.Size() != 4 ||
        !resultValue[0].IsString() || resultValue[0].GetStringLength() != 66 ||
        !resultValue[1].IsString() || resultValue[1].GetStringLength() != 66 ||
        !resultValue[2].IsString() || resultValue[2].GetStringLength() != 66 ||
        !resultValue[3].IsString())
      break;

    BaseBlob<256> headerHash;
    BaseBlob<256> seedHash;
    headerHash.setHexLE(resultValue[0].GetString() + 2);
    seedHash.setHexLE(resultValue[1].GetString() + 2);
    std::reverse(seedHash.begin(), seedHash.end());

    // TODO optimize it
    UInt<256> target;
    static UInt<256> twoPow255 = UInt<256>::fromHex("8000000000000000000000000000000000000000000000000000000000000000");
    target.setHex(resultValue[2].GetString() + 2);
    UInt<256> diff = twoPow255 / target;
    difficulty = diff.getDouble() * 2.0;

    height = strtoul(resultValue[3].GetString()+2, nullptr, 16);
    // Use height as unique block identifier
    workId = height;

    // Check DAG presence
    int epochNumber = ethashGetEpochNumber(seedHash.begin());
    if (epochNumber == -1) {
      CLOG_FC(*LogChannel_, ERROR, "Can't find epoch number for seed {}", seedHash.getHexLE());
      break;
    }

    // For ETC
    if (CoinInfo_.BigEpoch)
      epochNumber /= 2;

    blockTemplate->Height = static_cast<int64_t>(height);
    blockTemplate->DagFile = Dispatcher_->backend()->dagFile(epochNumber);
    if (blockTemplate->DagFile.get() != nullptr)
      templateIsOk = true;

    Dispatcher_->backend()->updateDag(epochNumber, CoinInfo_.BigEpoch);
    break;
  }

  if (templateIsOk) {
    if (WorkFetcher_.WorkId != workId) {
      Dispatcher_->onWorkFetcherNewWork(blockTemplate.release());
      WorkFetcher_.Height = height;
      WorkFetcher_.WorkId = workId;
      CLOG_FC(*LogChannel_, INFO, "{}: new work available; height: {}; difficulty: {}", CoinInfo_.Name, height, difficulty);
    }

    // Wait 100ms
    userEventStartTimer(WorkFetcher_.TimerEvent, 1*100000, 1);
  } else {
    httpClientDelete(WorkFetcher_.Client);
    Dispatcher_->onWorkFetcherConnectionLost();
  }

}

void CEthereumRpcClient::onWorkFetchTimeout()
{
  aioHttpRequest(WorkFetcher_.Client, EthGetWork_.data<const char>(), EthGetWork_.sizeOf(), 10000000, httpParseDefault, &WorkFetcher_.ParseCtx, [](AsyncOpStatus status, HTTPClient*, void *arg){
    static_cast<CEthereumRpcClient*>(arg)->onWorkFetcherIncomingData(status);
  }, this);
}

CEthereumRpcClient::CConnection *CEthereumRpcClient::getConnection(asyncBase *base)
{
  CConnection *connection = new CConnection;
  connection->Socket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  // NOTE: Linux only
  if (connection->Socket == -1) {
    CLOG_F(ERROR, "Can't create socket (open file descriptors limit is over?)");
    return nullptr;
  }
  connection->Client = httpClientNew(base, newSocketIo(base, connection->Socket));
  return connection;
}

int64_t CEthereumRpcClient::ioSearchUncle(CConnection *connection, int64_t height, const std::string &mixHash, int64_t bestBlockHeight, std::string &publicHash)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));
  int64_t maxHeight = std::min(height + 16, bestBlockHeight);

  for (int64_t currentHeight = height; currentHeight <= maxHeight; currentHeight++) {
    ETHBlock currentBlock;
    if (ethGetBlockByNumber(connection, currentHeight, currentBlock) != EStatusOk)
      return false;

    for (unsigned uncleIdx = 0; uncleIdx < currentBlock.Uncles.size(); uncleIdx++) {
      ETHBlock uncleBlock;
      if (ethGetUncleByBlockNumberAndIndex(connection, currentHeight, uncleIdx, uncleBlock) != EStatusOk)
        return false;

      if (uncleBlock.MixHash == UInt<256>::fromHex(mixHash.c_str())) {
        publicHash = uncleBlock.Hash.getHex(true, false, false);
        return currentHeight;
      }
    }
  }

  return 0;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGetBalance(CConnection *connection, const std::string &address, UInt<384> *balance)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  jsonStream.reset();
  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_getBalance");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
      paramsArray.addString(address);
      paramsArray.addString("latest");
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000);
  if (status != EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsString() || document["result"].GetStringLength() < 3)
    return EStatusProtocolError;

  *balance = fromWeiHex(document["result"].GetString());
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGasPrice(CConnection *connection, UInt<384> *gasPrice)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  jsonStream.reset();
  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_gasPrice");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000);
  if (status != EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsString() || document["result"].GetStringLength() < 3)
    return EStatusProtocolError;

  *gasPrice = fromWeiHex(document["result"].GetString());
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethMaxPriorityFeePerGas(CConnection *connection, UInt<384> *maxPriorityFeePerGas)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  jsonStream.reset();
  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_maxPriorityFeePerGas");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000);
  if (status != EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsString() || document["result"].GetStringLength() < 3)
    return EStatusProtocolError;

  *maxPriorityFeePerGas = fromWeiHex(document["result"].GetString());
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGetTransactionCount(CConnection *connection, const std::string &address, uint64_t *count)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  jsonStream.reset();
  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_getTransactionCount");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
      paramsArray.addString(address);
      paramsArray.addString("pending");
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000);
  if (status != EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsString() || document["result"].GetStringLength() < 3)
    return EStatusProtocolError;
  *count = strtoull(document["result"].GetString() + 2, nullptr, 16);
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethBlockNumber(CConnection *connection, uint64_t *blockNumber)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  jsonStream.reset();
  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_blockNumber");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000);
  if (status != EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsString() || document["result"].GetStringLength() < 3)
    return EStatusProtocolError;

  *blockNumber = strtoull(document["result"].GetString() + 2, nullptr, 16);
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGetBlockByNumber(CConnection *connection, uint64_t height, ETHBlock &block)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));
  jsonStream.reset();
  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_getBlockByNumber");
    queryObject.addField("params");
    {
      char hex[64];
      snprintf(hex, sizeof(hex), "0x%" PRIx64 "", height);
      JSON::Array paramsArray(jsonStream);
      paramsArray.addString(hex);
      paramsArray.addBoolean(true);
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 60*1000000);
  if (status != EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsObject()) {
    CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
    return EStatusProtocolError;
  }

  rapidjson::Value &blockObject = document["result"].GetObject();
  if (!blockObject.HasMember("mixHash") || !blockObject["mixHash"].IsString() || blockObject["mixHash"].GetStringLength() != 66 ||
      !blockObject.HasMember("hash") || !blockObject["hash"].IsString() || blockObject["hash"].GetStringLength() != 66 ||
      !blockObject.HasMember("gasUsed") || !blockObject["gasUsed"].IsString() || blockObject["gasUsed"].GetStringLength() < 3) {
    CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
    return EStatusProtocolError;
  }

  block.Hash = UInt<256>::fromHex(blockObject["hash"].GetString());
  block.MixHash = UInt<256>::fromHex(blockObject["mixHash"].GetString());
  block.GasUsed = strtoull(blockObject["gasUsed"].GetString() + 2, nullptr, 16);

  if (blockObject.HasMember("baseFeePerGas")) {
    if (!blockObject["baseFeePerGas"].IsString() || blockObject["gasUsed"].GetStringLength() < 3) {
      CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
      return EStatusProtocolError;
    }

    block.BaseFeePerGas = fromWeiHex(blockObject["baseFeePerGas"].GetString());
  }

  // transactions
  if (!blockObject.HasMember("transactions") || !blockObject["transactions"].IsArray()) {
    CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
    return EStatusProtocolError;
  }

  for (const auto &txObject: blockObject["transactions"].GetArray()) {
    // Here we need hash and gas price
    if (!txObject.HasMember("gasPrice") || !txObject["gasPrice"].IsString() || txObject["gasPrice"].GetStringLength() < 3 ||
        !txObject.HasMember("hash") || !txObject["hash"].IsString() || txObject["hash"].GetStringLength() != 66) {
      CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
      return EStatusProtocolError;
    }

    // use gwei for transaction fee
    ETHTransaction &tx = block.Transactions.emplace_back();
    tx.GasPrice = fromWeiHex(txObject["gasPrice"].GetString());
    tx.Hash = UInt<256>::fromHex(txObject["hash"].GetString());
  }

  // uncles
  if (!blockObject.HasMember("uncles") || !blockObject["uncles"].IsArray()) {
    CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
    return EStatusProtocolError;
  }

  for (const auto &uncle: blockObject["uncles"].GetArray()) {
    if (!uncle.IsString() || uncle.GetStringLength() != 66) {
      CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
      return EStatusProtocolError;
    }

    block.Uncles.emplace_back(UInt<256>::fromHex(uncle.GetString()));
  }

  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGetUncleByBlockNumberAndIndex(CConnection *connection, uint64_t height, unsigned uncleIndex, ETHBlock &block)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));
  jsonStream.reset();
  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_getUncleByBlockNumberAndIndex");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
      paramsArray.addIntHex(height, false, true);
      paramsArray.addIntHex(uncleIndex, false, true);
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000);
  if (status != EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsObject()) {
    CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
    return EStatusProtocolError;
  }

  rapidjson::Value &blockObject = document["result"].GetObject();
  if (!blockObject.HasMember("mixHash") || !blockObject["mixHash"].IsString() || blockObject["mixHash"].GetStringLength() != 66 ||
      !blockObject.HasMember("hash") || !blockObject["hash"].IsString() || blockObject["hash"].GetStringLength() != 66 ||
      !blockObject.HasMember("gasUsed") || !blockObject["gasUsed"].IsString() || blockObject["gasUsed"].GetStringLength() < 3) {
    CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
    return EStatusProtocolError;
  }

  block.Hash = UInt<256>::fromHex(blockObject["hash"].GetString());
  block.MixHash = UInt<256>::fromHex(blockObject["mixHash"].GetString());
  block.GasUsed = strtoull(blockObject["gasUsed"].GetString() + 2, nullptr, 16);

  if (blockObject.HasMember("baseFeePerGas")) {
    if (!blockObject["baseFeePerGas"].IsString() || blockObject["gasUsed"].GetStringLength() < 3) {
      CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
      return EStatusProtocolError;
    }

    block.BaseFeePerGas = fromWeiHex(blockObject["baseFeePerGas"].GetString());
  }

  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGetTransactionByHash(CConnection *connection, const UInt<256> &txid, ETHTransaction &tx)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));
  jsonStream.reset();

  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_getTransactionByHash");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
      paramsArray.addString(txid.getHex(true, true, false));
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000);
  if (status != EStatusOk)
    return status;

  if (!document.HasMember("result"))
    return CNetworkClient::EStatusProtocolError;
  if (document["result"].IsNull())
    return EStatusInvalidAddressOrKey;
  else if (!document["result"].IsObject())
    return CNetworkClient::EStatusProtocolError;
  const auto &resultObject = document["result"];

  if (!resultObject.HasMember("gasPrice") || !resultObject["gasPrice"].IsString() || resultObject["gasPrice"].GetStringLength() < 3)
    return CNetworkClient::EStatusProtocolError;

  tx.GasPrice = fromWeiHex(resultObject["gasPrice"].GetString());
  tx.Hash = txid;
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethGetTransactionReceipt(CConnection *connection, const UInt<256> &txid, ETHTransactionReceipt &receipt)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));
  jsonStream.reset();

  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_getTransactionReceipt");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
      paramsArray.addString(txid.getHex(true, true, false));
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000);
  if (status != EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsObject()) {
    CLOG_F(WARNING, "{} {}: response invalid format", CoinInfo_.Name, FullHostName_);
    return EStatusProtocolError;
  }

  const auto &resultObject = document["result"];
  if (!resultObject.HasMember("gasUsed") || !resultObject["gasUsed"].IsString() || resultObject["gasUsed"].GetStringLength() < 3 ||
      !resultObject.HasMember("blockNumber") || !resultObject["blockNumber"].IsString() || resultObject["blockNumber"].GetStringLength() < 3)
    return EStatusProtocolError;

  receipt.GasUsed = strtoull(resultObject["gasUsed"].GetString() + 2, nullptr, 16);
  receipt.BlockNumber = strtoull(resultObject["blockNumber"].GetString() + 2, 0, 16);
  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethSignTransactionOld(CConnection *connection,
                                                                           const std::string &from,
                                                                           const std::string &to,
                                                                           const UInt<384> &value,
                                                                           uint64_t gas,
                                                                           const UInt<384> &gasPrice,
                                                                           uint64_t nonce,
                                                                           std::string &txData,
                                                                           std::string &txId)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  jsonStream.reset();
  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_signTransaction");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
      paramsArray.addField();
      {
        JSON::Object transaction(jsonStream);
        transaction.addString("from", from);
        transaction.addString("to", to);
        transaction.addString("value", toWeiHex(value));
        transaction.addIntHex("gas", gas, false, true);
        transaction.addString("gasPrice", toWeiHex(gasPrice));
        transaction.addIntHex("nonce", nonce, false, true);
      }
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000);
  if (status != EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsObject())
    return CNetworkClient::EStatusProtocolError;

  // get raw transaction
  const auto &resultObject = document["result"];
  if (!resultObject.HasMember("raw") || !resultObject["raw"].IsString() || resultObject["raw"].GetStringLength() < 4)
    return CNetworkClient::EStatusProtocolError;
  txData = resultObject["raw"].GetString() + 2;

  // get txid
  if (!resultObject.HasMember("tx") || !resultObject["tx"].IsObject())
    return CNetworkClient::EStatusProtocolError;
  const auto &txObject = resultObject["tx"];

  if (!txObject.HasMember("hash") || !txObject["hash"].IsString() || txObject["hash"].GetStringLength() < 4)
    return CNetworkClient::EStatusProtocolError;
  txId = txObject["hash"].GetString() + 2;

  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethSignTransaction1559(CConnection *connection,
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
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  jsonStream.reset();
  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_signTransaction");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
      paramsArray.addField();
      {
        JSON::Object transaction(jsonStream);
        transaction.addString("from", from);
        transaction.addString("to", to);
        transaction.addString("value", toWeiHex(value));
        transaction.addIntHex("gas", gas, false, true);
        transaction.addString("maxPriorityFeePerGas", toWeiHex(maxPriorityFeePerGas));
        transaction.addString("maxFeePerGas", toWeiHex(maxFeePerGas));
        transaction.addIntHex("nonce", nonce, false, true);
      }
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000);
  if (status != EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsObject())
    return CNetworkClient::EStatusProtocolError;

  // get raw transaction
  const auto &resultObject = document["result"];
  if (!resultObject.HasMember("raw") || !resultObject["raw"].IsString() || resultObject["raw"].GetStringLength() < 4)
    return CNetworkClient::EStatusProtocolError;
  txData = resultObject["raw"].GetString() + 2;

  // get txid
  if (!resultObject.HasMember("tx") || !resultObject["tx"].IsObject())
    return CNetworkClient::EStatusProtocolError;
  const auto &txObject = resultObject["tx"];

  if (!txObject.HasMember("hash") || !txObject["hash"].IsString() || txObject["hash"].GetStringLength() < 4)
    return CNetworkClient::EStatusProtocolError;
  txId = txObject["hash"].GetString() + 2;

  return EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ethSendRawTransaction(CConnection *connection, const std::string &txData)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));
  jsonStream.reset();
  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_sendRawTransaction");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
      paramsArray.addString("0x" + txData);
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000);
  if (status != EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsString())
    return CNetworkClient::EStatusProtocolError;

  return CNetworkClient::EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::personalUnlockAccount(CConnection *connection,
                                                                           const std::string &address,
                                                                           const std::string &passPhrase,
                                                                           unsigned seconds)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));
  jsonStream.reset();
  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "personal_unlockAccount");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
      paramsArray.addString(address);
      paramsArray.addString(passPhrase);
      paramsArray.addInt(seconds);
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 180*1000000);
  if (status != CNetworkClient::EStatusOk)
    return status;

  if (!document.HasMember("result") || !document["result"].IsBool())
    return EStatusProtocolError;

  if (!document["result"].IsTrue()) {
    connection->LastError = "Can't unlock wallet";
    return CNetworkClient::EStatusUnknownError;
  }

  return EStatusOk;
}
