#include "poolcommon/httpClient.h"
#include "asyncio/asyncio.h"
#include "asyncio/http.h"
#include "asyncio/socket.h"
#include "asyncio/base64.h"
#include "p2putils/uriParse.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>
#include <type_traits>

static const char* httpMethodString(HttpMethod method)
{
  switch (method) {
    case HttpMethod::GET: return "GET";
    case HttpMethod::HEAD: return "HEAD";
    case HttpMethod::POST: return "POST";
    case HttpMethod::PUT: return "PUT";
    case HttpMethod::DELETE: return "DELETE";
    case HttpMethod::PATCH: return "PATCH";
  }
  return "GET";
}

static HttpResponse extractResponse(HTTPParseDefaultContext &ctx)
{
  HttpResponse resp;
  resp.StatusCode = ctx.resultCode;
  if (ctx.contentType.data)
    resp.ContentType.assign(ctx.contentType.data, ctx.contentType.size);
  if (ctx.body.data)
    resp.Body.assign(ctx.body.data, ctx.body.size);
  return resp;
}

// Internal state for aio (callback-based) requests.
// Self-destructs in the final callback — no leak.
struct AioOperation {
  HTTPClient *Client = nullptr;
  HTTPParseDefaultContext ParseCtx;
  std::string RawRequest;
  HttpResponseCb Callback;
  uint64_t Timeout;

  AioOperation() { httpParseDefaultInit(&ParseCtx); }

  ~AioOperation()
  {
    dynamicBufferFree(&ParseCtx.buffer);
    if (Client)
      httpClientDelete(Client);
  }

  static void onConnect(AsyncOpStatus status, HTTPClient*, void *arg)
  {
    auto *op = static_cast<AioOperation*>(arg);
    if (status != aosSuccess) {
      op->Callback(status, HttpResponse{});
      delete op;
      return;
    }

    aioHttpRequest(op->Client, op->RawRequest.c_str(), op->RawRequest.size(),
                   op->Timeout, httpParseDefault, &op->ParseCtx,
                   onResponse, op);
  }

  static void onResponse(AsyncOpStatus status, HTTPClient*, void *arg)
  {
    auto *op = static_cast<AioOperation*>(arg);
    if (status != aosSuccess)
      op->Callback(status, HttpResponse{});
    else
      op->Callback(aosSuccess, extractResponse(op->ParseCtx));
    delete op;
  }
};

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

template<typename ConnectionPolicy>
CHttpClient<ConnectionPolicy>::CHttpClient(asyncBase *base, const char *url)
  : Base_(base)
{
  URI uri;
  if (!uriParse(url, &uri))
    return;

  UseTls_ = (uri.schema == "https");
  uint16_t defaultPort = UseTls_ ? 443 : 80;
  uint16_t port = uri.port ? static_cast<uint16_t>(uri.port) : defaultPort;

  if (!uri.domain.empty()) {
    struct hostent *host = gethostbyname(uri.domain.c_str());
    if (host && host->h_addr_list && host->h_addr_list[0]) {
      Address_.ipv4 = reinterpret_cast<struct in_addr*>(host->h_addr_list[0])->s_addr;
      Address_.port = htons(port);
      Address_.family = AF_INET;
    } else {
      return;
    }
    HostName_ = uri.domain;
  } else if (uri.hostType == URI::HostTypeIPv4) {
    Address_.ipv4 = uri.ipv4;
    Address_.port = htons(port);
    Address_.family = AF_INET;
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &uri.ipv4, ipStr, sizeof(ipStr));
    HostName_ = ipStr;
  } else {
    return;
  }

  if (port != defaultPort)
    HostName_ += ":" + std::to_string(port);

  if (!uri.path.empty() && uri.path != "/")
    BasePath_ = uri.path;
}

template<typename ConnectionPolicy>
CHttpClient<ConnectionPolicy>::CHttpClient(asyncBase *base, HostAddress address, bool useTls)
  : Base_(base), Address_(address), UseTls_(useTls)
{
  // Derive HostName_ from IP for the Host header.
  // Call setHostName() to override with a proper domain name.
  char ipStr[INET6_ADDRSTRLEN];
  if (address.family == AF_INET)
    inet_ntop(AF_INET, &address.ipv4, ipStr, sizeof(ipStr));
  else if (address.family == AF_INET6)
    inet_ntop(AF_INET6, address.ipv6, ipStr, sizeof(ipStr));
  else
    ipStr[0] = '\0';

  HostName_ = ipStr;
  uint16_t port = ntohs(address.port);
  uint16_t defaultPort = useTls ? 443 : 80;
  if (port != defaultPort)
    HostName_ += ":" + std::to_string(port);
}

// ---------------------------------------------------------------------------
// Destructor / move
// ---------------------------------------------------------------------------

template<typename ConnectionPolicy>
CHttpClient<ConnectionPolicy>::~CHttpClient() = default;

template<typename ConnectionPolicy>
CHttpClient<ConnectionPolicy>::CHttpClient(CHttpClient&& other) noexcept
  : Base_(other.Base_),
    Address_(other.Address_),
    UseTls_(other.UseTls_),
    HostName_(std::move(other.HostName_)),
    BasePath_(std::move(other.BasePath_)),
    BasicAuth_(std::move(other.BasicAuth_)),
    DefaultHeaders_(std::move(other.DefaultHeaders_)),
    DefaultTimeout_(other.DefaultTimeout_)
{
  other.Base_ = nullptr;
  other.Address_ = {};
}

template<typename ConnectionPolicy>
CHttpClient<ConnectionPolicy>& CHttpClient<ConnectionPolicy>::operator=(CHttpClient&& other) noexcept
{
  if (this != &other) {
    Base_ = other.Base_;
    Address_ = other.Address_;
    UseTls_ = other.UseTls_;
    HostName_ = std::move(other.HostName_);
    BasePath_ = std::move(other.BasePath_);
    BasicAuth_ = std::move(other.BasicAuth_);
    DefaultHeaders_ = std::move(other.DefaultHeaders_);
    DefaultTimeout_ = other.DefaultTimeout_;
    other.Base_ = nullptr;
    other.Address_ = {};
  }
  return *this;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

template<typename ConnectionPolicy>
void CHttpClient<ConnectionPolicy>::setBasicAuth(const char *login, const char *password)
{
  std::string credentials = login;
  credentials.push_back(':');
  credentials.append(password);
  BasicAuth_.resize(base64getEncodeLength(credentials.size()));
  base64Encode(BasicAuth_.data(),
               reinterpret_cast<const uint8_t*>(credentials.data()),
               credentials.size());
}

template<typename ConnectionPolicy>
void CHttpClient<ConnectionPolicy>::setDefaultHeader(std::string name, std::string value)
{
  // Replace existing header with the same name
  for (auto &[n, v] : DefaultHeaders_) {
    if (n == name) {
      v = std::move(value);
      return;
    }
  }
  DefaultHeaders_.emplace_back(std::move(name), std::move(value));
}

template<typename ConnectionPolicy>
void CHttpClient<ConnectionPolicy>::setHostName(std::string hostName)
{
  HostName_ = std::move(hostName);
}

template<typename ConnectionPolicy>
void CHttpClient<ConnectionPolicy>::setDefaultTimeout(uint64_t timeoutUs)
{
  DefaultTimeout_ = timeoutUs;
}

// ---------------------------------------------------------------------------
// Raw HTTP request builder
// ---------------------------------------------------------------------------

template<typename ConnectionPolicy>
void CHttpClient<ConnectionPolicy>::buildRawRequest(const HttpRequest &request, std::string &out)
{
  out.clear();

  // Request line
  out.append(httpMethodString(request.Method));
  out.push_back(' ');
  out.append(BasePath_);
  out.append(request.Path);
  out.append(" HTTP/1.1\r\n");

  // Host
  out.append("Host: ");
  out.append(HostName_);
  out.append("\r\n");

  // Connection
  if constexpr (std::is_same_v<ConnectionPolicy, ConnectionPerRequest>)
    out.append("Connection: close\r\n");
  else
    out.append("Connection: keep-alive\r\n");

  // Authorization
  if (!BasicAuth_.empty()) {
    out.append("Authorization: Basic ");
    out.append(BasicAuth_);
    out.append("\r\n");
  }

  // Content-Type
  if (!request.ContentType.empty()) {
    out.append("Content-Type: ");
    out.append(request.ContentType);
    out.append("\r\n");
  }

  // Content-Length
  if (!request.Body.empty()) {
    out.append("Content-Length: ");
    out.append(std::to_string(request.Body.size()));
    out.append("\r\n");
  }

  // Default headers
  for (const auto &[name, value] : DefaultHeaders_) {
    out.append(name);
    out.append(": ");
    out.append(value);
    out.append("\r\n");
  }

  // Per-request headers
  for (const auto &[name, value] : request.Headers) {
    out.append(name);
    out.append(": ");
    out.append(value);
    out.append("\r\n");
  }

  // End of headers
  out.append("\r\n");

  // Body
  if (!request.Body.empty())
    out.append(request.Body);
}

// ---------------------------------------------------------------------------
// ioRequest — synchronous, for coroutines
// ---------------------------------------------------------------------------

template<typename ConnectionPolicy>
AsyncOpStatus CHttpClient<ConnectionPolicy>::ioRequest(const HttpRequest &request, HttpResponse &response)
{
  if (!isValid())
    return aosUnknownError;

  std::string rawRequest;
  buildRawRequest(request, rawRequest);

  uint64_t timeout = effectiveTimeout(request);

  // Create connection
  HTTPClient *client;
  if (UseTls_) {
    SSLSocket *sslSocket = sslSocketNew(Base_, nullptr);
    client = httpsClientNew(Base_, sslSocket);
  } else {
    socketTy sock = socketCreate(Address_.family, SOCK_STREAM, IPPROTO_TCP, 1);
    aioObject *socketObj = newSocketIo(Base_, sock);
    client = httpClientNew(Base_, socketObj);
  }

  // Connect
  const char *tlsHost = UseTls_ ? HostName_.c_str() : nullptr;
  int connectResult = ioHttpConnect(client, &Address_, tlsHost, timeout);
  if (connectResult != 0) {
    httpClientDelete(client);
    return static_cast<AsyncOpStatus>(-connectResult);
  }

  // Send request and receive response
  HTTPParseDefaultContext parseCtx;
  httpParseDefaultInit(&parseCtx);

  AsyncOpStatus status = ioHttpRequest(client, rawRequest.c_str(), rawRequest.size(),
                                       timeout, httpParseDefault, &parseCtx);
  if (status == aosSuccess)
    response = extractResponse(parseCtx);

  dynamicBufferFree(&parseCtx.buffer);
  httpClientDelete(client);
  return status;
}

// ---------------------------------------------------------------------------
// aioRequest — asynchronous, callback-based
// ---------------------------------------------------------------------------

template<typename ConnectionPolicy>
void CHttpClient<ConnectionPolicy>::aioRequest(const HttpRequest &request, HttpResponseCb callback)
{
  if (!isValid()) {
    callback(aosUnknownError, HttpResponse{});
    return;
  }

  auto *op = new AioOperation();
  op->Callback = std::move(callback);
  op->Timeout = effectiveTimeout(request);
  buildRawRequest(request, op->RawRequest);

  // Create connection
  if (UseTls_) {
    SSLSocket *sslSocket = sslSocketNew(Base_, nullptr);
    op->Client = httpsClientNew(Base_, sslSocket);
  } else {
    socketTy sock = socketCreate(Address_.family, SOCK_STREAM, IPPROTO_TCP, 1);
    aioObject *socketObj = newSocketIo(Base_, sock);
    op->Client = httpClientNew(Base_, socketObj);
  }

  // Start async connect
  const char *tlsHost = UseTls_ ? HostName_.c_str() : nullptr;
  aioHttpConnect(op->Client, &Address_, tlsHost, op->Timeout,
                 AioOperation::onConnect, op);
}

// ---------------------------------------------------------------------------
// Explicit template instantiation
// ---------------------------------------------------------------------------

template class CHttpClient<ConnectionPerRequest>;
