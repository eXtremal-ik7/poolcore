#include "poolcommon/httpClient.h"
#include "asyncio/asyncio.h"
#include "asyncio/http.h"
#include "asyncio/socket.h"
#include "asyncio/base64.h"
#include "p2putils/uriParse.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <atomic>
#include <cstring>

static const char *httpMethodString(HttpMethod method)
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

// Thread-local pool of parse buffers: avoids malloc/free of the ~65 KB
// dynamicBuffer on every HTTP request.  Grows to the high-water mark of
// concurrent requests and stays there.
static thread_local std::vector<dynamicBuffer> tlsParseBuffers;

static void parseCtxInit(HTTPParseDefaultContext *ctx)
{
  if (!tlsParseBuffers.empty()) {
    ctx->buffer = tlsParseBuffers.back();
    ctx->buffer.offset = 0;
    ctx->buffer.size = 0;
    tlsParseBuffers.pop_back();
  } else {
    dynamicBufferInit(&ctx->buffer, 65536);
  }
}

static constexpr size_t MaxCachedParseBuffers = 16;

static void parseCtxFree(HTTPParseDefaultContext *ctx)
{
  if (tlsParseBuffers.size() < MaxCachedParseBuffers) {
    tlsParseBuffers.push_back(ctx->buffer);
  } else {
    dynamicBufferFree(&ctx->buffer);
  }
  ctx->buffer = {};
}

static HTTPClient *createConnection(asyncBase *base, const HostAddress &address, bool useTls)
{
  socketTy sock = socketCreate(address.family, SOCK_STREAM, IPPROTO_TCP, 1);
  if (sock < 0)
    return nullptr;
  aioObject *socketObj = newSocketIo(base, sock);
  if (useTls) {
    SSLSocket *sslSocket = sslSocketNew(base, socketObj);
    return httpsClientNew(base, sslSocket);
  } else {
    return httpClientNew(base, socketObj);
  }
}

// ---------------------------------------------------------------------------
// Per-request state for CHttpEndpoint async operations.
// Owns the HTTPClient. Self-destructs in the final callback.
// ---------------------------------------------------------------------------

struct AioOperation {
  HTTPClient *Client = nullptr;
  HTTPParseDefaultContext ParseCtx;
  std::string RawRequest;
  HttpResponseCb Callback;
  uint64_t Timeout;

  AioOperation() { parseCtxInit(&ParseCtx); }

  ~AioOperation() {
    parseCtxFree(&ParseCtx);
    if (Client)
      httpClientDelete(Client);
  }

  static void onConnect(AsyncOpStatus status, HTTPClient*, void *arg) {
    auto *op = static_cast<AioOperation*>(arg);
    if (status != aosSuccess) {
      op->Callback(status, HttpResponse{});
      delete op;
      return;
    }

    aioHttpRequest(op->Client, op->RawRequest.c_str(), op->RawRequest.size(), op->Timeout, httpParseDefault, &op->ParseCtx, onResponse, op);
  }

  static void onResponse(AsyncOpStatus status, HTTPClient*, void *arg) {
    auto *op = static_cast<AioOperation*>(arg);
    if (status != aosSuccess)
      op->Callback(status, HttpResponse{});
    else
      op->Callback(aosSuccess, extractResponse(op->ParseCtx));
    delete op;
  }
};

// ---------------------------------------------------------------------------
// Keep-alive action codes
// ---------------------------------------------------------------------------

static constexpr uint8_t KA_REQUEST = 0;
static constexpr uint8_t KA_CONNECT_DONE = 1;
static constexpr uint8_t KA_DISCONNECT = 2;
static constexpr uint8_t KA_DELETE = 3;

// ---------------------------------------------------------------------------
// Per-request state for CHttpConnection async operations.
// Linked into combiner CAS-stack via Next pointer.
//
// Each async request carries its own HTTPParseDefaultContext because asyncio
// defers finish callbacks to a global queue — the next op's httpParseStart
// reinitializes the parse context before the previous callback runs.
// ---------------------------------------------------------------------------

struct KeepAliveOp {
  KeepAliveOp *Next = nullptr;      // combiner chain link
  KeepAliveState *Owner = nullptr;  // ref-counted state
  uint8_t Action = 0;               // KA_REQUEST / KA_CONNECT_DONE / KA_DISCONNECT
  AsyncOpStatus ConnectStatus{};    // for KA_CONNECT_DONE
  HTTPClient *ForClient = nullptr;  // for KA_DISCONNECT: skip if Client changed (nullptr = unconditional)
  std::string RawRequest;           // for KA_REQUEST
  HttpResponseCb Callback;          // for KA_REQUEST
  uint64_t Timeout = 0;             // for KA_REQUEST
  HTTPParseDefaultContext ParseCtx{}; // for KA_REQUEST (per-op)
};

// Sentinel: combiner is active, no new ops pending.
// Uses address 1 (guaranteed not a valid heap pointer due to alignment).
static KeepAliveOp *const COMBINER_SENTINEL = reinterpret_cast<KeepAliveOp*>(uintptr_t(1));

// ---------------------------------------------------------------------------
// KeepAliveState — ref-counted keep-alive connection state.
//
// Ref-counting follows the asyncio pattern:
//   - CHttpConnection holds one ref (released in destructor)
//   - Each KeepAliveOp holds one per-op ref (addRef at creation, release
//     at deletion) — analogous to asyncio's initAsyncOpRoot/releaseAsyncOp
//   - Connect callback holds one ref (between aioHttpConnect and callback)
//   - Deletion goes through the combiner (KA_DELETE), so combinerRun never
//     touches freed memory — no addRef/release needed around combinerRun
// ---------------------------------------------------------------------------

struct KeepAliveState {
  std::atomic<unsigned> RefCount{1};
  std::atomic<KeepAliveOp*> CombinerHead{nullptr};

  // Connection state (combiner-protected)
  HTTPClient *Client = nullptr;
  bool Connecting = false;
  std::vector<KeepAliveOp*> PendingConnect;

  // Embedded op for delete-through-combiner (never heap-allocated)
  KeepAliveOp DeleteOp_;

  // Config (copied from CHttpBase at construction, immutable)
  asyncBase *Base = nullptr;
  HostAddress Address{};
  bool UseTls = false;
  std::string TlsHost; // domain only, for SNI (empty for IP addresses)

  KeepAliveState(asyncBase *base, HostAddress address, bool useTls, std::string tlsHost)
    : Base(base), Address(address), UseTls(useTls), TlsHost(std::move(tlsHost))
  {}

  void addRef() { RefCount.fetch_add(1, std::memory_order_relaxed); }

  // Release one ref. When the last ref is gone, route cleanup through
  // the combiner (like asyncio's COMBINER_TAG_DELETE) so that an active
  // combinerRun never touches freed memory.
  void release()
  {
    if (RefCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      DeleteOp_.Action = KA_DELETE;
      DeleteOp_.Owner = this;
      combinerPush(&DeleteOp_);
    }
  }

  // Batch-release `count` refs (single atomic). Used inside combinerRun
  // when multiple ops are deleted in one combinerProcessOp call.
  void releaseMultiple(unsigned count)
  {
    if (RefCount.fetch_sub(count, std::memory_order_acq_rel) == count) {
      DeleteOp_.Action = KA_DELETE;
      DeleteOp_.Owner = this;
      combinerPush(&DeleteOp_);
    }
  }

  // Disconnect and free PendingConnect ops. Returns the number of
  // per-op refs that the caller must release (one per freed op).
  unsigned disconnectInternal()
  {
    unsigned freedOps = 0;
    if (auto *client = Client) {
      Client = nullptr;
      Connecting = false;
      httpClientDelete(client);
    }

    for (auto *op: PendingConnect) {
      if (op->Callback)
        op->Callback(aosUnknownError, HttpResponse{});
      parseCtxFree(&op->ParseCtx);
      delete op;
      freedOps++;
    }
    PendingConnect.clear();
    return freedOps;
  }

  // --- Flat combining (same pattern as asyncio) ---

  void combinerPush(KeepAliveOp *op)
  {
    KeepAliveOp *head;
    do {
      head = CombinerHead.load(std::memory_order_acquire);
      op->Next = head;
    } while (!CombinerHead.compare_exchange_weak(head, op, std::memory_order_acq_rel, std::memory_order_acquire));

    if (head == nullptr)
      combinerRun();
  }

  void combinerRun()
  {
    KeepAliveOp *batch = CombinerHead.exchange(COMBINER_SENTINEL, std::memory_order_acq_rel);

    for (;;) {
      if (batch == COMBINER_SENTINEL || batch == nullptr) {
        KeepAliveOp *expected = COMBINER_SENTINEL;
        if (CombinerHead.compare_exchange_strong(expected, nullptr, std::memory_order_release, std::memory_order_acquire))
          return;
        batch = CombinerHead.exchange(COMBINER_SENTINEL, std::memory_order_acq_rel);
        continue;
      }

      // Reverse for FIFO order (CAS stack is LIFO)
      KeepAliveOp *reversed = nullptr;
      while (batch && batch != COMBINER_SENTINEL) {
        KeepAliveOp *next = batch->Next;
        batch->Next = reversed;
        reversed = batch;
        batch = next;
      }

      while (reversed) {
        KeepAliveOp *next = reversed->Next;
        if (combinerProcessOp(reversed))
          return; // KA_DELETE: state destroyed, bail out
        reversed = next;
      }

      batch = CombinerHead.exchange(COMBINER_SENTINEL, std::memory_order_acq_rel);
    }
  }

  // Returns true if KA_DELETE was processed (state destroyed, caller must exit).
  bool combinerProcessOp(KeepAliveOp *op)
  {
    switch (op->Action) {
      case KA_REQUEST:
        if (!Client) {
          Client = createConnection(Base, Address, UseTls);
          if (!Client) {
            if (op->Callback)
              op->Callback(aosUnknownError, HttpResponse{});
            parseCtxFree(&op->ParseCtx);
            delete op;
            releaseMultiple(1);
            break;
          }
          Connecting = true;
          PendingConnect.push_back(op);
          const char *tlsHost = (UseTls && !TlsHost.empty()) ? TlsHost.c_str() : nullptr;
          addRef(); // ref for connect callback
          aioHttpConnect(
              Client,
              &Address,
              tlsHost,
              op->Timeout,
              [](AsyncOpStatus status, HTTPClient*, void *arg) {
                auto *state = static_cast<KeepAliveState*>(arg);
                state->addRef(); // per-op ref for KA_CONNECT_DONE
                auto *resultOp = new KeepAliveOp{};
                resultOp->Action = KA_CONNECT_DONE;
                resultOp->ConnectStatus = status;
                resultOp->Owner = state;
                state->combinerPush(resultOp);
                state->release(); // release connect callback ref
              },
              this);
        } else if (Connecting) {
          PendingConnect.push_back(op);
        } else {
          submitToAsyncio(op);
        }
        break;

      case KA_CONNECT_DONE: {
        Connecting = false;
        auto pending = std::move(PendingConnect);
        unsigned freed = 0;

        if (op->ConnectStatus != aosSuccess) {
          freed += disconnectInternal(); // PendingConnect already moved — only handles Client
          for (auto *p: pending) {
            if (p->Callback)
              p->Callback(op->ConnectStatus, HttpResponse{});
            parseCtxFree(&p->ParseCtx);
            delete p;
            freed++;
          }
        } else {
          for (auto *p: pending)
            submitToAsyncio(p);
        }
        delete op;
        freed++; // per-op ref for KA_CONNECT_DONE
        releaseMultiple(freed); // may push KA_DELETE
        break;
      }

      case KA_DISCONNECT: {
        unsigned freed = 0;
        if (!op->ForClient || op->ForClient == Client)
          freed = disconnectInternal();
        delete op;
        freed++; // per-op ref for KA_DISCONNECT
        releaseMultiple(freed); // may push KA_DELETE
        break;
      }

      case KA_DELETE:
        disconnectInternal();
        delete this;
        return true;
    }
    return false;
  }

  void submitToAsyncio(KeepAliveOp *op)
  {
    aioHttpRequest(
        Client,
        op->RawRequest.c_str(),
        op->RawRequest.size(),
        op->Timeout,
        httpParseDefault,
        &op->ParseCtx,
        [](AsyncOpStatus status, HTTPClient *client, void *arg) {
          auto *op = static_cast<KeepAliveOp*>(arg);
          op->Owner->onKeepAliveResponse(status, op, client);
        },
        op);
  }

  void onKeepAliveResponse(AsyncOpStatus status, KeepAliveOp *op, HTTPClient *failedClient)
  {
    auto cb = std::move(op->Callback);
    HttpResponse resp;

    if (status == aosSuccess) {
      resp = extractResponse(op->ParseCtx);
      parseCtxFree(&op->ParseCtx);
      delete op;
      release(); // per-op ref; may trigger KA_DELETE
      cb(aosSuccess, std::move(resp));
    } else {
      parseCtxFree(&op->ParseCtx);
      // Create disconnect op (with per-op ref) BEFORE releasing original
      // to prevent a RefCount→0 gap.
      addRef(); // per-op ref for KA_DISCONNECT
      auto *dop = new KeepAliveOp{};
      dop->Action = KA_DISCONNECT;
      dop->Owner = this;
      dop->ForClient = failedClient; // skip if connection already replaced
      delete op;
      release(); // per-op ref for original op
      combinerPush(dop);
      cb(status, std::move(resp));
    }
  }
};

// ===========================================================================
// CHttpBase
// ===========================================================================

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

CHttpBase::CHttpBase(const char *url)
{
  URI uri;
  if (!uriParse(url, &uri))
    return;

  UseTls_ = (uri.schema == "https");
  uint16_t defaultPort = UseTls_ ? 443 : 80;
  uint16_t port = uri.port ? static_cast<uint16_t>(uri.port) : defaultPort;

  if (!uri.domain.empty()) {
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(uri.domain.c_str(), nullptr, &hints, &result) != 0 || !result) {
      return;
    }
    if (result->ai_family == AF_INET) {
      Address_.ipv4 = reinterpret_cast<struct sockaddr_in*>(result->ai_addr)->sin_addr.s_addr;
      Address_.family = AF_INET;
    } else if (result->ai_family == AF_INET6) {
      memcpy(Address_.ipv6, &reinterpret_cast<struct sockaddr_in6*>(result->ai_addr)->sin6_addr, sizeof(Address_.ipv6));
      Address_.family = AF_INET6;
    } else {
      freeaddrinfo(result);
      return;
    }
    freeaddrinfo(result);
    Address_.port = htons(port);
    TlsHost_ = uri.domain;
    HostName_ = uri.domain;
  } else if (uri.hostType == URI::HostTypeIPv4) {
    Address_.ipv4 = uri.ipv4;
    Address_.port = htons(port);
    Address_.family = AF_INET;
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &uri.ipv4, ipStr, sizeof(ipStr));
    HostName_ = ipStr;
  } else if (uri.hostType == URI::HostTypeIPv6) {
    memcpy(Address_.ipv6, uri.ipv6, sizeof(Address_.ipv6));
    Address_.port = htons(port);
    Address_.family = AF_INET6;
    char ipStr[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, uri.ipv6, ipStr, sizeof(ipStr));
    HostName_ = "[";
    HostName_ += ipStr;
    HostName_ += "]";
  } else {
    return;
  }

  if (port != defaultPort)
    HostName_ += ":" + std::to_string(port);

  if (!uri.path.empty() && uri.path != "/")
    BasePath_ = uri.path;
}

CHttpBase::CHttpBase(HostAddress address, bool useTls) :
  Address_(address),
  UseTls_(useTls)
{
  char ipStr[INET6_ADDRSTRLEN];
  if (address.family == AF_INET) {
    inet_ntop(AF_INET, &address.ipv4, ipStr, sizeof(ipStr));
    HostName_ = ipStr;
  } else if (address.family == AF_INET6) {
    inet_ntop(AF_INET6, address.ipv6, ipStr, sizeof(ipStr));
    HostName_ = "[";
    HostName_ += ipStr;
    HostName_ += "]";
  } else {
    HostName_.clear();
  }

  uint16_t port = ntohs(address.port);
  uint16_t defaultPort = useTls ? 443 : 80;
  if (port != defaultPort)
    HostName_ += ":" + std::to_string(port);

  // TlsHost_ stays empty — call setHostName() for domain-based TLS SNI
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void CHttpBase::setBasicAuth(const char *login, const char *password)
{
  std::string credentials = login;
  credentials.push_back(':');
  credentials.append(password);
  BasicAuth_.resize(base64getEncodeLength(credentials.size()) + 1); // +1 for null written by base64Encode
  size_t encodedLen = base64Encode(BasicAuth_.data(), reinterpret_cast<const uint8_t*>(credentials.data()), credentials.size());
  BasicAuth_.resize(encodedLen);
}

void CHttpBase::setDefaultHeader(std::string name, std::string value)
{
  for (auto& [n, v]: DefaultHeaders_) {
    if (n == name) {
      v = std::move(value);
      return;
    }
  }
  DefaultHeaders_.emplace_back(std::move(name), std::move(value));
}

void CHttpBase::setHostName(std::string hostName)
{
  TlsHost_ = hostName;
  HostName_ = std::move(hostName);
  uint16_t port = ntohs(Address_.port);
  uint16_t defaultPort = UseTls_ ? 443 : 80;
  if (port != defaultPort)
    HostName_ += ":" + std::to_string(port);
}

void CHttpBase::setDefaultTimeout(uint64_t timeoutUs)
{
  DefaultTimeout_ = timeoutUs;
}

// ---------------------------------------------------------------------------
// prepare — build raw HTTP request text
// ---------------------------------------------------------------------------

std::string CHttpBase::prepare(const HttpRequest &request) const
{
  std::string out;
  buildRawRequest(request, out);
  return out;
}

// ---------------------------------------------------------------------------
// Raw HTTP request builder
// ---------------------------------------------------------------------------

void CHttpBase::buildRawRequest(const HttpRequest &request, std::string &out) const
{
  out.clear();

  out.append(httpMethodString(request.Method));
  out.push_back(' ');
  out.append(BasePath_);
  out.append(request.Path);
  out.append(" HTTP/1.1\r\n");

  out.append("Host: ");
  out.append(HostName_);
  out.append("\r\n");

  if (KeepAlive_)
    out.append("Connection: keep-alive\r\n");
  else
    out.append("Connection: close\r\n");

  if (!BasicAuth_.empty()) {
    out.append("Authorization: Basic ");
    out.append(BasicAuth_);
    out.append("\r\n");
  }

  if (!request.ContentType.empty()) {
    out.append("Content-Type: ");
    out.append(request.ContentType);
    out.append("\r\n");
  }

  bool hasBody = request.Method == HttpMethod::POST || request.Method == HttpMethod::PUT || request.Method == HttpMethod::PATCH;
  if (hasBody || !request.Body.empty()) {
    out.append("Content-Length: ");
    out.append(std::to_string(request.Body.size()));
    out.append("\r\n");
  }

  for (const auto& [name, value]: DefaultHeaders_) {
    out.append(name);
    out.append(": ");
    out.append(value);
    out.append("\r\n");
  }

  for (const auto& [name, value]: request.Headers) {
    out.append(name);
    out.append(": ");
    out.append(value);
    out.append("\r\n");
  }

  out.append("\r\n");

  if (!request.Body.empty())
    out.append(request.Body);
}

// ===========================================================================
// CHttpEndpoint
// ===========================================================================

AsyncOpStatus CHttpEndpoint::ioRequest(asyncBase *base, std::string request, HttpResponse &response, uint64_t timeout)
{
  if (!isValid())
    return aosUnknownError;

  timeout = effectiveTimeout(timeout);

  HTTPClient *client = createConnection(base, Address_, UseTls_);
  if (!client)
    return aosUnknownError;
  const char *tlsHost = (UseTls_ && !TlsHost_.empty()) ? TlsHost_.c_str() : nullptr;
  int connectError = ioHttpConnect(client, &Address_, tlsHost, timeout);
  if (connectError != 0) {
    httpClientDelete(client);
    return static_cast<AsyncOpStatus>(-connectError);
  }

  HTTPParseDefaultContext parseCtx;
  parseCtxInit(&parseCtx);

  AsyncOpStatus status = ioHttpRequest(client, request.c_str(), request.size(), timeout, httpParseDefault, &parseCtx);
  if (status == aosSuccess)
    response = extractResponse(parseCtx);

  parseCtxFree(&parseCtx);
  httpClientDelete(client);
  return status;
}

void CHttpEndpoint::aioRequest(asyncBase *base, std::string request, HttpResponseCb callback, uint64_t timeout)
{
  if (!isValid()) {
    callback(aosUnknownError, HttpResponse{});
    return;
  }

  timeout = effectiveTimeout(timeout);

  auto *op = new AioOperation();
  op->Callback = std::move(callback);
  op->Timeout = timeout;
  op->RawRequest = std::move(request);
  op->Client = createConnection(base, Address_, UseTls_);
  if (!op->Client) {
    op->Callback(aosUnknownError, HttpResponse{});
    delete op;
    return;
  }

  const char *tlsHost = (UseTls_ && !TlsHost_.empty()) ? TlsHost_.c_str() : nullptr;
  aioHttpConnect(op->Client, &Address_, tlsHost, op->Timeout, AioOperation::onConnect, op);
}

// ===========================================================================
// CHttpConnection
// ===========================================================================

CHttpConnection::CHttpConnection(asyncBase *base, const char *url) :
  CHttpBase(url),
  Base_(base)
{
  KeepAlive_ = true;
  if (isValid())
    State_ = new KeepAliveState(base, Address_, UseTls_, TlsHost_);
}

CHttpConnection::CHttpConnection(asyncBase *base, HostAddress address, bool useTls) :
  CHttpBase(address, useTls),
  Base_(base)
{
  KeepAlive_ = true;
  if (isValid())
    State_ = new KeepAliveState(base, Address_, UseTls_, TlsHost_);
}

CHttpConnection::~CHttpConnection()
{
  if (State_) {
    State_->release();
    State_ = nullptr;
  }
}

void CHttpConnection::setHostName(std::string hostName)
{
  CHttpBase::setHostName(std::move(hostName));
  if (State_)
    State_->TlsHost = TlsHost_;
}

AsyncOpStatus CHttpConnection::ioRequest(std::string request, HttpResponse &response, uint64_t timeout)
{
  if (!isValid())
    return aosUnknownError;

  timeout = effectiveTimeout(timeout);

  AsyncOpStatus syncStatus;
  coroutineTy *current = coroutineCurrent();

  State_->addRef(); // per-op ref
  auto *op = new KeepAliveOp{};
  op->Owner = State_;
  op->Action = KA_REQUEST;
  op->Callback = [&syncStatus, &response, current](AsyncOpStatus status, HttpResponse resp) {
    syncStatus = status;
    response = std::move(resp);
    coroutineCall(current);
  };
  op->Timeout = timeout;
  op->RawRequest = std::move(request);
  parseCtxInit(&op->ParseCtx);
  State_->combinerPush(op);
  coroutineYield();
  return syncStatus;
}

void CHttpConnection::aioRequest(std::string request, HttpResponseCb callback, uint64_t timeout)
{
  if (!isValid()) {
    callback(aosUnknownError, HttpResponse{});
    return;
  }

  timeout = effectiveTimeout(timeout);

  State_->addRef(); // per-op ref
  auto *op = new KeepAliveOp{};
  op->Owner = State_;
  op->Action = KA_REQUEST;
  op->Callback = std::move(callback);
  op->Timeout = timeout;
  op->RawRequest = std::move(request);
  parseCtxInit(&op->ParseCtx);
  State_->combinerPush(op);
}
