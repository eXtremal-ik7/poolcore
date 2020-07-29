#include "poolinstances/stratumMsg.h"
#include "poolcommon/utils.h"
#include "rapidjson/document.h"
#include "p2putils/strExtras.h"
#include "loguru.hpp"
#include "p2putils/xmstream.h"

StratumDecodeStatusTy decodeStratumMessage(const char *in, size_t size, StratumMessage *out)
{
  rapidjson::Document document;
  document.Parse(in, size);
  if (document.HasParseError()) {
    return JsonError;
  }

  if (!(document.HasMember("id") && document.HasMember("method") && document.HasMember("params")))
    return FormatError;
  if (!(document["method"].IsString() && document["params"].IsArray()))
    return FormatError;

  if (document["id"].IsUint64())
    out->integerId = document["id"].GetUint64();
  else if (document["id"].IsString())
    out->stringId = document["id"].GetString();
  else
    return FormatError;

  std::string method = document["method"].GetString();
  const rapidjson::Value::Array &params = document["params"].GetArray();
  if (method == "mining.subscribe" && params.Size() >= 1) {
    out->method = Subscribe;
    if (params[0].IsString())
      out->subscribe.minerUserAgent = params[0].GetString();
    else
      return FormatError;

    if (params.Size() >= 2) {
      if (params[1].IsString())
        out->subscribe.sessionId = params[1].GetString();
      else
        return FormatError;
    } else if (params.Size() >= 3) {
      if (params[2].IsString())
        out->subscribe.connectHost = params[2].GetString();
      else
        return FormatError;
    } else if (params.Size() >= 4) {
      if (params[3].IsUint())
        out->subscribe.connectPort = params[3].GetUint();
      else
        return FormatError;
    }
  } else if (method == "mining.authorize" && params.Size() >= 2) {
    out->method = Authorize;
    if (params[0].IsString() && params[1].IsString()) {
      out->authorize.login = params[0].GetString();
      out->authorize.password = params[1].GetString();
    } else {
      return FormatError;
    }
  } else if (method == "mining.extranonce.subscribe") {
    out->method = ExtraNonceSubscribe;
  } else if (method == "mining.submit" && params.Size() >= 5) {
    if (params[0].IsString() &&
        params[1].IsString() &&
        params[2].IsString() &&
        params[3].IsString() && params[3].GetStringLength() == 8 &&
        params[4].IsString() && params[4].GetStringLength() == 8) {
      out->method = Submit;
      out->submit.WorkerName = params[0].GetString();
      out->submit.JobId = params[1].GetString();
      {
        // extra nonce mutable part
        out->submit.MutableExtraNonce.resize(params[2].GetStringLength() / 2);
        hex2bin(params[2].GetString(), params[2].GetStringLength(), out->submit.MutableExtraNonce.data());
      }
      out->submit.Time = readHexBE<uint32_t>(params[3].GetString(), 4);
      out->submit.Nonce = readHexBE<uint32_t>(params[4].GetString(), 4);
    } else {
      return FormatError;
    }
  } else if (method == "mining.multi_version" && params.Size() >= 1) {
    out->method = MultiVersion;
    if (params[0].IsUint()) {
      out->multiVersion.Version = params[0].GetUint();
    } else {
      return FormatError;
    }
  } else {
    return FormatError;
  }

  return Ok;
}
