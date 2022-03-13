#include "poolcore/ethereumRPCClient.h"

#include "blockmaker/eth.h"
#include "blockmaker/ethash.h"
#include "poolcore/backend.h"
#include "poolcore/blockTemplate.h"
#include "poolcore/clientDispatcher.h"
#include "poolcommon/jsonSerializer.h"
#include "poolcommon/arith_uint256.h"
#include "asyncio/asyncio.h"
#include "p2putils/uriParse.h"

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

static constexpr int64_t ByzantiumHeight = 4370000;
static constexpr int64_t ConstantinopleHeight = 7280000;

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
  WorkFetcherBase_(base), ThreadsNum_(threadsNum), CoinInfo_(coinInfo)
{
  WorkFetcher_.Client = nullptr;
  httpParseDefaultInit(&WorkFetcher_.ParseCtx);
  WorkFetcher_.TimerEvent = newUserEvent(base, 0, [](aioUserEvent*, void *arg) {
    static_cast<CEthereumRpcClient*>(arg)->onWorkFetchTimeout();
  }, this);

  URI uri;
  std::string uriAddress = (std::string)"http://" + address;
  if (!uriParse(uriAddress.c_str(), &uri)) {
    LOG_F(ERROR, "%s: can't parse address %s", coinInfo.Name.c_str(), address);
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

  if (config.MiningAddresses.size() != 1) {
    LOG_F(ERROR, "ERROR: ethereum-based backends support working with only one mining address\n");
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
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return false;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
    return false;


  jsonStream.reset();
  {
    JSON::Object queryObject(jsonStream);
    queryObject.addString("jsonrpc", "2.0");
    queryObject.addString("method", "eth_getBalance");
    queryObject.addField("params");
    {
      JSON::Array paramsArray(jsonStream);
      paramsArray.addString(MiningAddress_);
      paramsArray.addString("latest");
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  if (ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000) != EStatusOk)
    return false;

  if (!document.HasMember("result") ||
      !document["result"].IsString() ||
      document["result"].GetStringLength() <= 2)
    return false;

  arith_uint256 balance;
  arith_uint256 gwei(static_cast<uint64_t>(1000000000ULL));
  balance.SetHex(document["result"].GetString());
  balance /= gwei;
  result.Balance = balance.GetLow64();
  result.Immatured = 0;
  return true;
}

bool CEthereumRpcClient::ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockConfirmationsQuery> &queries)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  for (auto &It: queries)
    It.Confirmations = -2;

  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return false;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
    return false;

  // First, get best chain height
  int64_t bestBlockHeight = 0;
  {
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
    if (ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000) != EStatusOk)
      return false;

    if (!document.HasMember("result") ||
        !document["result"].IsString() ||
        document["result"].GetStringLength() <= 2)
      return false;

    bestBlockHeight = strtoul(document["result"].GetString() + 2, nullptr, 16);
  }

  LOG_F(WARNING, "best block: %lli\n", bestBlockHeight);

  for (auto &query: queries) {
    // Process each block separately
    jsonStream.reset();
    {
      JSON::Object queryObject(jsonStream);
      queryObject.addString("jsonrpc", "2.0");
      queryObject.addString("method", "eth_getBlockByNumber");
      queryObject.addField("params");
      {
        char hex[64];
        snprintf(hex, sizeof(hex), "0x%" PRIx64 "", query.Height);
        JSON::Array paramsArray(jsonStream);
        paramsArray.addString(hex);
        paramsArray.addBoolean(true);
      }
      queryObject.addInt("id", -1);
    }

    rapidjson::Document document;
    if (ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000) != EStatusOk)
      return false;

    if (!document.HasMember("result") || !document["result"].IsObject()) {
      LOG_F(WARNING, "%s %s: response invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
      return false;
    }
    rapidjson::Value &blockObject = document["result"].GetObject();

    std::string publicHash;
    if (matchBlock(blockObject, query.Hash, publicHash)) {
      query.Confirmations = bestBlockHeight - query.Height;
    } else {
      std::string publicHash;
      int64_t uncleHeight = ioSearchUncle(connection.get(), query.Height, query.Hash, bestBlockHeight, publicHash);
      if (uncleHeight) {
        query.Confirmations = bestBlockHeight - uncleHeight;
      } else if (bestBlockHeight - query.Height < orphanAgeLimit) {
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
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  for (auto &It: queries)
    It.Confirmations = -2;

  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return false;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
    return false;

  // First, get best chain height
  int64_t bestBlockHeight = 0;
  {
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
    if (ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000) != EStatusOk)
      return false;

    if (!document.HasMember("result") ||
        !document["result"].IsString() ||
        document["result"].GetStringLength() <= 2)
      return false;

    bestBlockHeight = strtoul(document["result"].GetString() + 2, nullptr, 16);
  }

  LOG_F(WARNING, "best block: %lli\n", bestBlockHeight);


  for (auto &query: queries) {
    // Process each block separately
    jsonStream.reset();
    {
      JSON::Object queryObject(jsonStream);
      queryObject.addString("jsonrpc", "2.0");
      queryObject.addString("method", "eth_getBlockByNumber");
      queryObject.addField("params");
      {
        char hex[64];
        snprintf(hex, sizeof(hex), "0x%" PRIx64 "", query.Height);
        JSON::Array paramsArray(jsonStream);
        paramsArray.addString(hex);
        paramsArray.addBoolean(true);
      }
      queryObject.addInt("id", -1);
    }

    rapidjson::Document document;
    if (ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000) != EStatusOk)
      return false;

    if (!document.HasMember("result") || !document["result"].IsObject()) {
      LOG_F(WARNING, "%s %s: response invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
      return false;
    }

    rapidjson::Value &blockObject = document["result"].GetObject();

    if (!matchBlock(blockObject, query.Hash, query.PublicHash)) {
      std::string publicHash;
      int64_t uncleHeight = ioSearchUncle(connection.get(), query.Height, query.Hash, bestBlockHeight, query.PublicHash);
      if (uncleHeight) {
        query.Confirmations = bestBlockHeight - uncleHeight;
        query.BlockReward = getConstBlockReward(uncleHeight) * (8 - (uncleHeight-query.Height))/8;
      } else if (bestBlockHeight - query.Height < orphanAgeLimit) {
        query.Confirmations = -3;
      } else {
        query.Confirmations = -1;
      }

      LOG_F(WARNING, "height %lli; hash: %s; reward: %s; confirmations: %lli\n", query.Height, query.PublicHash.c_str(), FormatMoney(query.BlockReward, 1000000000LL).c_str(), query.Confirmations);
      continue;
    }

    // Get block reward
    int64_t constReward = getConstBlockReward(query.Height);
    LOG_F(WARNING, "const reward: %lli\n", constReward);

    // Get tx fee
    int64_t totalTxFee = query.TxFee;
    if (totalTxFee == 0) {
      if (!blockObject.HasMember("transactions") || !blockObject["transactions"].IsArray()) {
        LOG_F(WARNING, "%s %s: response invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
        return false;
      }

      const auto &transactionsArray = blockObject["transactions"].GetArray();

      for (const auto &txObject: transactionsArray) {
        // Here we need hash and gas price
        if (!txObject.HasMember("gasPrice") || !txObject["gasPrice"].IsString() || txObject["gasPrice"].GetStringLength() <= 2 ||
            !txObject.HasMember("hash") || !txObject["hash"].IsString() || txObject["hash"].GetStringLength() != 66) {
          LOG_F(WARNING, "%s %s: response invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
          return false;
        }

        // use gwei for transaction fee
        int64_t txFee = 0;
        int64_t heightUnUsed = 0;
        int64_t gasPrice = strtoll(txObject["gasPrice"].GetString() + 2, nullptr, 16);
        if (!getTxStatus(connection.get(), txObject["hash"].GetString(), gasPrice, &txFee, &heightUnUsed))
          return false;

        totalTxFee += txFee;
      }

      LOG_F(WARNING, "all tx fee: %lli\n", totalTxFee);
    }

    // Get uncles reward
    int64_t unclesReward = 0;
    if (blockObject.HasMember("uncles") && blockObject["uncles"].IsArray())
      unclesReward = (constReward / 32) * blockObject["uncles"].GetArray().Size();

    // Get gas fee
    int64_t gasFee = 0;
    if (blockObject.HasMember("gasUsed") && blockObject["gasUsed"].IsString() && blockObject["gasUsed"].GetStringLength() >= 3 &&
        blockObject.HasMember("baseFeePerGas") && blockObject["baseFeePerGas"].IsString() && blockObject["baseFeePerGas"].GetStringLength() >= 3) {
      int64_t gasUsed = strtoll(blockObject["gasUsed"].GetString() + 2, nullptr, 16);
      int64_t baseFeePerGas = strtoll(blockObject["baseFeePerGas"].GetString() + 2, nullptr, 16);
      gasFee = gasUsed * baseFeePerGas / 1000000000LL;
    }

    int64_t blockReward = constReward + unclesReward + totalTxFee - gasFee;
    query.TxFee = totalTxFee;
    query.BlockReward = blockReward;
    query.Confirmations = bestBlockHeight - query.Height;
    LOG_F(WARNING, "height %lli; hash: %s; reward: %s; confirmations: %lli\n", query.Height, query.PublicHash.c_str(), FormatMoney(blockReward, 1000000000LL).c_str(), query.Confirmations);
  }

  return true;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ioBuildTransaction(asyncBase *base, const std::string &address, const std::string&, const int64_t value, BuildTransactionResult &result)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return CNetworkClient::EStatusNetworkError;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
    return CNetworkClient::EStatusNetworkError;

  // Check balance first
  // We require payout value + 5% at balance
  {
    jsonStream.reset();
    {
      JSON::Object queryObject(jsonStream);
      queryObject.addString("jsonrpc", "2.0");
      queryObject.addString("method", "eth_getBalance");
      queryObject.addField("params");
      {
        JSON::Array paramsArray(jsonStream);
        paramsArray.addString(MiningAddress_);
        paramsArray.addString("latest");
      }
      queryObject.addInt("id", -1);
    }

    rapidjson::Document document;
    CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 180*1000000);
    if (status != CNetworkClient::EStatusOk) {
      result.Error = connection->LastError;
      return status;
    }

    if (!document.HasMember("result") || !document["result"].IsString() || document["result"].GetStringLength() < 4)
      return CNetworkClient::EStatusProtocolError;

    arith_uint256 balance;
    arith_uint256 gwei(static_cast<uint64_t>(1000000000ULL));
    balance.SetHex(document["result"].GetString());
    balance /= gwei;
    int64_t balanceGwei = balance.GetLow64();
    if (balanceGwei < (value+value/20)) {
      return CNetworkClient::EStatusInsufficientFunds;
    }
  }

  // We need those values:
  //   * base fee per gas (eth.maxPriorityFeePerGas - eth.gasPrice)
  //   * max priority fee per gas (eth.maxPriorityFeePerGas)
  //   * max fee per gas (eth.maxPriorityFeePerGas + 2*baseFeePerGas)
  //   * nonce (eth.getTransactionCount)
  int64_t gasPrice = 0;
  int64_t baseFeePerGas = 0;
  int64_t maxPriorityFeePerGas = 0;
  int64_t maxFeePerGas = 0;
  int64_t nonce = 0;

  {
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
    CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 180*1000000);
    if (status != CNetworkClient::EStatusOk) {
      result.Error = connection->LastError;
      return status;
    }

    if (!document.HasMember("result") || !document["result"].IsString() || document["result"].GetStringLength() <= 2)
      return CNetworkClient::EStatusProtocolError;
    gasPrice = strtoll(document["result"].GetString() + 2, nullptr, 16);
  }

  {
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
    CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 180*1000000);
    if (status != CNetworkClient::EStatusOk) {
      result.Error = connection->LastError;
      return status;
    }

    if (!document.HasMember("result") || !document["result"].IsString() || document["result"].GetStringLength() <= 2)
      return CNetworkClient::EStatusProtocolError;
    maxPriorityFeePerGas = strtoll(document["result"].GetString() + 2, nullptr, 16);
  }

  baseFeePerGas = gasPrice - maxPriorityFeePerGas;
  maxFeePerGas = maxPriorityFeePerGas + 2*baseFeePerGas;

  {
    jsonStream.reset();
    {
      JSON::Object queryObject(jsonStream);
      queryObject.addString("jsonrpc", "2.0");
      queryObject.addString("method", "eth_getTransactionCount");
      queryObject.addField("params");
      {
        JSON::Array paramsArray(jsonStream);
        paramsArray.addString(MiningAddress_);
        paramsArray.addString("pending");
      }
      queryObject.addInt("id", -1);
    }

    rapidjson::Document document;
    CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 180*1000000);
    if (status != CNetworkClient::EStatusOk) {
      result.Error = connection->LastError;
      return status;
    }

    if (!document.HasMember("result") || !document["result"].IsString() || document["result"].GetStringLength() <= 2)
      return CNetworkClient::EStatusProtocolError;
    nonce = strtoll(document["result"].GetString() + 2, nullptr, 16);
  }

  // Unlock account
  {
    jsonStream.reset();
    {
      JSON::Object queryObject(jsonStream);
      queryObject.addString("jsonrpc", "2.0");
      queryObject.addString("method", "personal_unlockAccount");
      queryObject.addField("params");
      {
        JSON::Array paramsArray(jsonStream);
        paramsArray.addString(MiningAddress_);
        paramsArray.addString("");
        paramsArray.addInt(60u);
      }
      queryObject.addInt("id", -1);
    }

    rapidjson::Document document;
    CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 180*1000000);
    if (status != CNetworkClient::EStatusOk) {
      result.Error = connection->LastError;
      return status;
    }

    if (!document.HasMember("result") || !document["result"].IsBool() || !document["result"].IsTrue()) {
      result.Error = "Can't unlock wallet";
      return CNetworkClient::EStatusUnknownError;
    }
  }

  // convert value to Wei
  std::string valueInWeiHexTrimmed = "0x";
  {
    arith_uint256 valueInWei(static_cast<uint64_t>(value));
    valueInWei *= 1000000000U;
    std::string valueInWeiHex = valueInWei.GetHex();
    const char *valueInWeiHexPtr = valueInWeiHex.c_str();
    while (*valueInWeiHexPtr == '0')
      valueInWeiHexPtr++;
    valueInWeiHexTrimmed += valueInWeiHexPtr;
  }


  // Build transaction
  {
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
          char gasHex[64];
          char maxPriorityFeePerGasHex[64];
          char maxFeePerGasHex[64];
          char nonceHex[64];
          snprintf(gasHex, sizeof(gasHex), "0x%X", 21000);
          snprintf(maxPriorityFeePerGasHex, sizeof(maxPriorityFeePerGasHex), "0x%" PRIx64 "", maxPriorityFeePerGas);
          snprintf(maxFeePerGasHex, sizeof(maxFeePerGasHex), "0x%" PRIx64 "", maxFeePerGas);
          snprintf(nonceHex, sizeof(nonceHex), "0x%" PRIx64 "", nonce);

          JSON::Object transaction(jsonStream);
          transaction.addString("from", MiningAddress_);
          transaction.addString("to", address);
          transaction.addString("value", valueInWeiHexTrimmed);
          transaction.addString("gas", gasHex);
          transaction.addString("maxPriorityFeePerGas", maxPriorityFeePerGasHex);
          transaction.addString("maxFeePerGas", maxFeePerGasHex);
          transaction.addString("nonce", nonceHex);
        }
      }
      queryObject.addInt("id", -1);
    }

    {
      jsonStream.write('\0');
    }

    rapidjson::Document document;
    CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 180*1000000);
    if (status != CNetworkClient::EStatusOk) {
      result.Error = connection->LastError;
      return status;
    }

    if (!document.HasMember("result") || !document["result"].IsObject())
      return CNetworkClient::EStatusProtocolError;

    // get raw transaction
    const auto &resultObject = document["result"];
    if (!resultObject.HasMember("raw") || !resultObject["raw"].IsString() || resultObject["raw"].GetStringLength() < 4)
      return CNetworkClient::EStatusProtocolError;
    auto txData = resultObject["raw"].GetString() + 2;

    // get txid
    if (!resultObject.HasMember("tx") || !resultObject["tx"].IsObject())
      return CNetworkClient::EStatusProtocolError;
    const auto &txObject = resultObject["tx"];

    if (!txObject.HasMember("hash") || !txObject["hash"].IsString() || txObject["hash"].GetStringLength() < 4)
      return CNetworkClient::EStatusProtocolError;
    auto txId = txObject["hash"].GetString() + 2;

    // Can't determine fee at this moment
    result.Fee = 0;
    result.TxData = txData;
    result.TxId = txId;
  }

  return CNetworkClient::EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ioSendTransaction(asyncBase *base, const std::string &txData, std::string &error)
{
  char buffer[4096];
  xmstream jsonStream(buffer, sizeof(buffer));

  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return CNetworkClient::EStatusNetworkError;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
    return CNetworkClient::EStatusNetworkError;

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
  CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 180*1000000);
  if (status != CNetworkClient::EStatusOk) {
    constexpr int RPC_INVALID_INPUT = -32000;

    error = connection->LastError;
    if (connection->LastErrorCode == RPC_INVALID_INPUT) {
      if (connection->LastError == "already known")
        return CNetworkClient::EStatusOk;
      else
        return EStatusVerifyRejected;
    }

    return status;
  }

  if (!document.HasMember("result") || !document["result"].IsString())
    return CNetworkClient::EStatusProtocolError;

  return CNetworkClient::EStatusOk;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, int64_t *txFee, std::string &error)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));

  std::unique_ptr<CConnection> connection(getConnection(base));
  if (!connection)
    return CNetworkClient::EStatusNetworkError;
  if (ioHttpConnect(connection->Client, &Address_, nullptr, 5000000) != 0)
    return CNetworkClient::EStatusNetworkError;

  int64_t gasPrice = 0;
  int64_t blockHeight = 0;

  {
    // eth_getTransaction - get actual gas price
    {
      JSON::Object queryObject(jsonStream);
      queryObject.addString("jsonrpc", "2.0");
      queryObject.addString("method", "eth_getTransactionByHash");
      queryObject.addField("params");
      {
        JSON::Array paramsArray(jsonStream);
        paramsArray.addString("0x" + txId);
      }
      queryObject.addInt("id", -1);
    }

    rapidjson::Document document;
    CNetworkClient::EOperationStatus status = ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 180*1000000);
    if (status != CNetworkClient::EStatusOk) {
      error = connection->LastError;
      return EStatusInvalidAddressOrKey;
    }

    if (!document.HasMember("result") || !document["result"].IsObject())
      return CNetworkClient::EStatusProtocolError;
    const auto &resultObject = document["result"];

    if (!resultObject.HasMember("gasPrice") || !resultObject["gasPrice"].IsString() || resultObject["gasPrice"].GetStringLength() < 4)
      return CNetworkClient::EStatusProtocolError;

    gasPrice = strtoll(resultObject["gasPrice"].GetString() + 2, nullptr, 16);
  }

  if (!getTxStatus(connection.get(), ("0x" + txId).c_str(), gasPrice, txFee, &blockHeight)) {
    error = connection->LastError;
    return EStatusInvalidAddressOrKey;
  }

  // get pending block
  int64_t bestBlockHeight = 0;
  {
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
    if (status != CNetworkClient::EStatusOk) {
      error = connection->LastError;
      return status;
    }

    if (!document.HasMember("result") || !document["result"].IsString() || document["result"].GetStringLength() < 4)
      return CNetworkClient::EStatusProtocolError;

    bestBlockHeight = strtoul(document["result"].GetString() + 2, nullptr, 16);
  }

  *confirmations = bestBlockHeight - blockHeight;
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

    uint256 headerHash;
    uint256 seedHash;
    headerHash.SetHex(resultValue[0].GetString() + 2);
    seedHash.SetHex(resultValue[1].GetString() + 2);
    std::reverse(seedHash.begin(), seedHash.end());

    // TODO optimize it
    arith_uint256 target;
    static arith_uint256 twoPow255("8000000000000000000000000000000000000000000000000000000000000000");
    target.SetHex(resultValue[2].GetString() + 2);
    arith_uint256 diff = twoPow255 / target;
    difficulty = diff.getdouble() * 2.0;

    height = strtoul(resultValue[3].GetString()+2, nullptr, 16);
    // Use height as unique block identifier
    workId = height;

    // Check DAG presence
    int epochNumber = ethashGetEpochNumber(seedHash.begin());
    if (epochNumber == -1) {
      LOG_F(ERROR, "Can't find epoch number for seed %s", seedHash.ToString().c_str());
      break;
    }

    // For ETC
    if (CoinInfo_.BigEpoch)
      epochNumber /= 2;

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
      LOG_F(INFO, "%s: new work available; height: %" PRIu64 "; difficulty: %lf", CoinInfo_.Name.c_str(), height, difficulty);
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
    LOG_F(ERROR, "Can't create socket (open file descriptors limit is over?)");
    return nullptr;
  }
  connection->Client = httpClientNew(base, newSocketIo(base, connection->Socket));
  return connection;
}

bool CEthereumRpcClient::matchBlock(const rapidjson::Value &block, const std::string &expectedMixHash, std::string &publicHash)
{
  // Check mix hash
  if (!block.HasMember("mixHash") || !block["mixHash"].IsString() || block["mixHash"].GetStringLength() != 66 ||
      !block.HasMember("hash") || !block["hash"].IsString() || block["hash"].GetStringLength() != 66) {
    LOG_F(WARNING, "%s %s: response invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
    return false;
  }

  char mixHashBin[32];
  char mixHashHexReversed[72] = {0};
  const char *blockMixHash = block["mixHash"].GetString() + 2;
  const char *blockHash = block["hash"].GetString() + 2;
  hex2bin(blockMixHash, 64*2, mixHashBin);
  std::reverse(mixHashBin, mixHashBin+32);
  bin2hexLowerCase(mixHashBin, mixHashHexReversed, 32);

  if (expectedMixHash == mixHashHexReversed) {
    publicHash = blockHash;
    return true;
  } else {
    return false;
  }
}

int64_t CEthereumRpcClient::ioSearchUncle(CConnection *connection, int64_t height, const std::string &mixHash, int64_t bestBlockHeight, std::string &publicHash)
{
  char buffer[1024];
  xmstream jsonStream(buffer, sizeof(buffer));
  int64_t maxHeight = std::min(height + 16, bestBlockHeight);

  for (int64_t currentHeight = height; currentHeight <= maxHeight; currentHeight++) {
    jsonStream.reset();
    {
      JSON::Object queryObject(jsonStream);
      queryObject.addString("jsonrpc", "2.0");
      queryObject.addString("method", "eth_getBlockByNumber");
      queryObject.addField("params");
      {
        char hex[64];
        snprintf(hex, sizeof(hex), "0x%" PRIx64 "", currentHeight);
        JSON::Array paramsArray(jsonStream);
        paramsArray.addString(hex);
        paramsArray.addBoolean(false);
      }
      queryObject.addInt("id", -1);
    }

    rapidjson::Document document;
    if (ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000) != EStatusOk)
      return false;

    if (!document.HasMember("result") || !document["result"].IsObject()) {
      LOG_F(WARNING, "%s %s: response invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
      return false;
    }

    rapidjson::Value &resultObject = document["result"].GetObject();
    if (!resultObject.HasMember("uncles") || !resultObject["uncles"].IsArray()) {
      LOG_F(WARNING, "%s %s: response invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
      return 0;
    }

    unsigned unclesNum = resultObject["uncles"].GetArray().Size();
    for (unsigned uncleIdx = 0; uncleIdx < unclesNum; uncleIdx++) {
      jsonStream.reset();
      {
        JSON::Object queryObject(jsonStream);
        queryObject.addString("jsonrpc", "2.0");
        queryObject.addString("method", "eth_getUncleByBlockNumberAndIndex");
        queryObject.addField("params");
        {
          char hex1[64];
          char hex2[64];
          snprintf(hex1, sizeof(hex1), "0x%" PRIx64 "", currentHeight);
          snprintf(hex2, sizeof(hex2), "0x%X", uncleIdx);
          JSON::Array paramsArray(jsonStream);
          paramsArray.addString(hex1);
          paramsArray.addString(hex2);
        }
        queryObject.addInt("id", -1);
      }

      rapidjson::Document uncleDocument;
      if (ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), uncleDocument, 5*1000000) != EStatusOk)
        return false;
      if (!uncleDocument.HasMember("result") || !uncleDocument["result"].IsObject()) {
        LOG_F(WARNING, "%s %s: response invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
        return false;
      }

      if (matchBlock(uncleDocument["result"].GetObject(), mixHash, publicHash)) {
        LOG_F(WARNING, "Uncle height: %lli, new height: %lli, public hash: %s\n", height, currentHeight, publicHash.c_str());
        return currentHeight;
      }
    }
  }

  return 0;
}

int64_t CEthereumRpcClient::getConstBlockReward(int64_t height)
{
  if (height < ByzantiumHeight)
    return 5 * 1000000000LL;
  else if (height < ConstantinopleHeight)
    return 3 * 1000000000LL;
  else
    return 2 * 1000000000LL;
}

bool CEthereumRpcClient::getTxStatus(CEthereumRpcClient::CConnection *connection, const char *txid, int64_t gasPrice, int64_t *txFee, int64_t *blockHeight)
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
      paramsArray.addString(txid);
    }
    queryObject.addInt("id", -1);
  }

  rapidjson::Document document;
  if (ioQueryJson(*connection, buildPostQuery("/", jsonStream.data<const char>(), jsonStream.sizeOf(), HostName_), document, 5*1000000) != EStatusOk)
    return false;

  if (!document.HasMember("result") || !document["result"].IsObject()) {
    LOG_F(WARNING, "%s %s: response invalid format", CoinInfo_.Name.c_str(), FullHostName_.c_str());
    return false;
  }

  const auto &resultObject = document["result"];
  if (!resultObject.HasMember("gasUsed") || !resultObject["gasUsed"].IsString() || resultObject["gasUsed"].GetStringLength() < 4 ||
      !resultObject.HasMember("blockNumber") || !resultObject["blockNumber"].IsString() || resultObject["blockNumber"].GetStringLength() < 4)
    return CNetworkClient::EStatusProtocolError;

  arith_uint256 txFee256(strtoull(resultObject["gasUsed"].GetString() + 2, nullptr, 16));
  arith_uint256 gasPrice256(static_cast<uint64_t>(gasPrice));
  txFee256 *= gasPrice256;
  txFee256 /= 1000000000ULL;

  *txFee = txFee256.GetLow64();
  *blockHeight = strtoll(resultObject["blockNumber"].GetString() + 2, nullptr, 16);
  return true;
}
