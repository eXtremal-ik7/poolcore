#pragma once

#include "asyncio/api.h"
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
struct ConnectionKeepAlive {}; // Not yet implemented

struct HttpRequest {
  HttpMethod Method = HttpMethod::GET;
  std::string_view Path = "/";
  std::string_view Body;
  std::string_view ContentType;
  uint64_t Timeout = 0; // 0 = use client default
  std::vector<std::pair<std::string_view, std::string_view>> Headers;
};

struct HttpResponse {
  unsigned StatusCode = 0;
  std::string ContentType;
  std::string Body;
  std::vector<std::pair<std::string, std::string>> Headers;
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

template<typename ConnectionPolicy = ConnectionPerRequest>
class CHttpClient {
  CHttpClient(const CHttpClient&) = delete;
  CHttpClient& operator=(const CHttpClient&) = delete;

public:
  // Construct from URL (e.g. "http://host:port/basePath" or "https://...")
  CHttpClient(asyncBase *base, const char *url);

  // Construct from HostAddress; call setHostName() for proper Host header / TLS SNI
  CHttpClient(asyncBase *base, HostAddress address, bool useTls = false);

  ~CHttpClient();

  CHttpClient(CHttpClient&&) noexcept;
  CHttpClient& operator=(CHttpClient&&) noexcept;

  // Configuration
  void setBasicAuth(const char *login, const char *password);
  void setDefaultHeader(std::string name, std::string value);
  void setHostName(std::string hostName);
  void setDefaultTimeout(uint64_t timeoutUs);

  bool isValid() const { return Address_.family != 0; }
  const std::string& hostName() const { return HostName_; }

  // --- Raw response ---
  AsyncOpStatus ioRequest(const HttpRequest &request, HttpResponse &response);
  void aioRequest(const HttpRequest &request, HttpResponseCb callback);

  // --- Parsed response (self-contained structs, no context) ---
  template<JsonParseable T>
  AsyncOpStatus ioRequest(const HttpRequest &request, T &result) {
    HttpResponse response;
    AsyncOpStatus status = ioRequest(request, response);
    if (status != aosSuccess)
      return status;
    if (!result.parse(response.Body.data(), response.Body.size()))
      return aosParseError;
    return aosSuccess;
  }

  template<JsonParseable T>
  void aioRequest(const HttpRequest &request, std::function<void(AsyncOpStatus, T)> callback) {
    aioRequest(request, [cb = std::move(callback)](AsyncOpStatus status, HttpResponse response) {
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
    });
  }

  // --- Parsed response (structs with Capture, user calls resolve) ---
  template<JsonParseableWithCapture T>
  AsyncOpStatus ioRequest(const HttpRequest &request, T &result, typename T::Capture &capture) {
    HttpResponse response;
    AsyncOpStatus status = ioRequest(request, response);
    if (status != aosSuccess)
      return status;
    if (!result.parse(response.Body.data(), response.Body.size(), capture))
      return aosParseError;
    return aosSuccess;
  }

  template<JsonParseableWithCapture T>
  void aioRequest(const HttpRequest &request,
                  std::function<void(AsyncOpStatus, T, typename T::Capture)> callback) {
    aioRequest(request, [cb = std::move(callback)](AsyncOpStatus status, HttpResponse response) {
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
    });
  }

private:
  void buildRawRequest(const HttpRequest &request, std::string &out);
  uint64_t effectiveTimeout(const HttpRequest &request) const {
    return request.Timeout ? request.Timeout : DefaultTimeout_;
  }

  asyncBase *Base_ = nullptr;
  HostAddress Address_{};
  bool UseTls_ = false;
  std::string HostName_;
  std::string BasePath_;
  std::string BasicAuth_; // Base64-encoded
  std::vector<std::pair<std::string, std::string>> DefaultHeaders_;
  uint64_t DefaultTimeout_ = 10000000; // 10 seconds in microseconds
};
