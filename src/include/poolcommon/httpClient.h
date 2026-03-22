#pragma once

#include "asyncio/api.h"
#include "asyncio/http.h"
#include <concepts>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

enum class HttpMethod {
  GET,
  HEAD,
  POST,
  PUT,
  DELETE,
  PATCH
};

// Connection policies (template parameter for CHttpClient)
struct ConnectionPerRequest {};
struct ConnectionKeepAlive {};

struct HttpRequest {
  HttpMethod Method = HttpMethod::GET;
  std::string_view Path = "/";
  std::string_view Body;
  std::string_view ContentType;
  std::vector<std::pair<std::string_view, std::string_view>> Headers;
};

struct HttpResponse {
  unsigned StatusCode = 0;
  std::string ContentType;
  std::string Body;
  std::vector<std::pair<std::string, std::string>> Headers; // TODO: not populated yet
};

// Extended status codes (after asyncio's aosLast)
constexpr AsyncOpStatus aosParseError = static_cast<AsyncOpStatus>(aosLast);

// Concepts for idltool-generated structs
template<typename T>
concept JsonParseable = requires(T t, const char *buf, size_t size) {
  { t.parse(buf, size) } -> std::same_as<bool>;
};

template<typename T>
concept JsonParseableWithCapture = requires(T t, const char *buf, size_t size, typename T::Capture &capture) {
  { t.parse(buf, size, capture) } -> std::same_as<bool>;
};

using HttpResponseCb = std::function<void(AsyncOpStatus, HttpResponse)>;

struct KeepAliveState; // ref-counted keep-alive state (defined in httpClient.cpp)

template<typename ConnectionPolicy = ConnectionPerRequest>
class CHttpClient {
  CHttpClient(const CHttpClient&) = delete;
  CHttpClient &operator=(const CHttpClient&) = delete;

public:
  // Construct from URL (e.g. "http://host:port/basePath" or "https://...")
  // Note: performs blocking DNS resolution (getaddrinfo) — call at init time,
  // not from the event loop or coroutines.
  CHttpClient(asyncBase *base, const char *url);

  // Construct from HostAddress; call setHostName() for proper Host header / TLS SNI
  CHttpClient(asyncBase *base, HostAddress address, bool useTls = false);

  ~CHttpClient();

  CHttpClient(CHttpClient&&) = delete;
  CHttpClient &operator=(CHttpClient&&) = delete;

  // Configuration (init-only: call before any ioRequest/aioRequest)
  void setBasicAuth(const char *login, const char *password);
  void setDefaultHeader(std::string name, std::string value);
  void setHostName(std::string hostName);
  void setDefaultTimeout(uint64_t timeoutUs);

  bool isValid() const { return Address_.family != 0; }
  const std::string &hostName() const { return HostName_; }

  // Keep-alive connection management
  void disconnect();

  // Build raw HTTP request text from HttpRequest
  std::string prepare(const HttpRequest &request) const;

  // --- Raw response ---
  AsyncOpStatus ioRequest(std::string request, HttpResponse &response, uint64_t timeout = 0);
  void aioRequest(std::string request, HttpResponseCb callback, uint64_t timeout = 0);

  // --- Parsed response (self-contained structs, no context) ---
  template<JsonParseable T>
  AsyncOpStatus ioRequest(std::string request, T &result, uint64_t timeout = 0) {
    HttpResponse response;
    AsyncOpStatus status = ioRequest(std::move(request), response, timeout);
    if (status != aosSuccess)
      return status;
    if (!result.parse(response.Body.data(), response.Body.size()))
      return aosParseError;
    return aosSuccess;
  }

  template<JsonParseable T>
  void aioRequest(std::string request, std::function<void(AsyncOpStatus, T)> callback, uint64_t timeout = 0) {
    aioRequest(std::move(request), [cb = std::move(callback)](AsyncOpStatus status, HttpResponse response) {
      if (status != aosSuccess) {
        cb(status, T{});
        return;
      }
      T result;
      if (!result.parse(response.Body.data(), response.Body.size())) {
        cb(aosParseError, T{});
        return;
      }
      cb(aosSuccess, std::move(result));
    }, timeout);
  }

  // --- Parsed response (structs with Capture, user calls resolve) ---
  template<JsonParseableWithCapture T>
  AsyncOpStatus ioRequest(std::string request, T &result, typename T::Capture &capture, uint64_t timeout = 0) {
    HttpResponse response;
    AsyncOpStatus status = ioRequest(std::move(request), response, timeout);
    if (status != aosSuccess)
      return status;
    if (!result.parse(response.Body.data(), response.Body.size(), capture))
      return aosParseError;
    return aosSuccess;
  }

  template<JsonParseableWithCapture T>
  void aioRequest(std::string request, std::function<void(AsyncOpStatus, T, typename T::Capture)> callback, uint64_t timeout = 0) {
    aioRequest(std::move(request), [cb = std::move(callback)](AsyncOpStatus status, HttpResponse response) {
      if (status != aosSuccess) {
        cb(status, T{}, typename T::Capture{});
        return;
      }
      T result;
      typename T::Capture capture;
      if (!result.parse(response.Body.data(), response.Body.size(), capture)) {
        cb(aosParseError, T{}, typename T::Capture{});
        return;
      }
      cb(aosSuccess, std::move(result), std::move(capture));
    }, timeout);
  }

private:
  void buildRawRequest(const HttpRequest &request, std::string &out) const;
  uint64_t effectiveTimeout(uint64_t timeout) const { return timeout ? timeout : DefaultTimeout_; }

  asyncBase *Base_ = nullptr;
  HostAddress Address_{};
  bool UseTls_ = false;
  std::string HostName_;  // Host header value (with :port and [brackets] for IPv6)
  std::string TlsHost_;  // TLS SNI hostname (domain only, no port; empty for IP addresses)
  std::string BasePath_;
  std::string BasicAuth_; // Base64-encoded
  std::vector<std::pair<std::string, std::string>> DefaultHeaders_;
  uint64_t DefaultTimeout_ = 10000000; // 10 seconds in microseconds

  // Ref-counted keep-alive state shared between CHttpClient and in-flight
  // asyncio callbacks. CHttpClient holds one ref; each submitted async op
  // holds one ref. The state is freed when the last ref is released.
  KeepAliveState *State_ = nullptr;
};
