#include "poolcore/ethereumRPCClient.h"

#include "blockmaker/eth.h"
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

CEthereumRpcClient::CEthereumRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, PoolBackendConfig&) : CNetworkClient(threadsNum),
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
  return false;
}

bool CEthereumRpcClient::ioGetBlockConfirmations(asyncBase *base, std::vector<GetBlockConfirmationsQuery> &query)
{
  return false;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const int64_t value, BuildTransactionResult &result)
{
  return CNetworkClient::EStatusUnknownError;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ioSendTransaction(asyncBase *base, const std::string &txData, std::string &error)
{
  return CNetworkClient::EStatusUnknownError;
}

CNetworkClient::EOperationStatus CEthereumRpcClient::ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, std::string &error)
{
  return CNetworkClient::EStatusUnknownError;
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

    headerHash.SetHex(resultValue[0].GetString() + 2);

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
    unsigned epochNumber = height / 30000;
    blockTemplate->DagFile = Dispatcher_->backend()->dagFile(epochNumber);
    if (blockTemplate->DagFile.get() != nullptr)
      templateIsOk = true;

    Dispatcher_->backend()->updateDag(epochNumber);
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
