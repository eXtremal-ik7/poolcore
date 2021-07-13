#include "blockmaker/eth.h"

namespace ETH {
EStratumDecodeStatusTy Stratum::StratumMessage::decodeStratumMessage(const char *in, size_t size)
{
  rapidjson::Document document;
  document.Parse(in, size);
  if (document.HasParseError()) {
    return EStratumStatusJsonError;
  }

  if (!(document.HasMember("id") && document.HasMember("method") && document.HasMember("params")))
    return EStratumStatusFormatError;

  // Some clients put null to 'params' field
  if (document["params"].IsNull())
    document["params"].SetArray();

  if (!(document["method"].IsString() && document["params"].IsArray()))
    return EStratumStatusFormatError;

  if (document["id"].IsUint64())
    IntegerId = document["id"].GetUint64();
  else if (document["id"].IsString())
    StringId = document["id"].GetString();
  else
    return EStratumStatusFormatError;

  std::string method = document["method"].GetString();
  const rapidjson::Value::Array &params = document["params"].GetArray();
  if (method == "mining.subscribe") {
    Method = ESubscribe;
    if (params.Size() >= 1) {
      if (params[0].IsString())
        Subscribe.minerUserAgent = params[0].GetString();
    }

    if (params.Size() >= 2) {
      if (params[1].IsString())
        Subscribe.StratumVersion = params[1].GetString();
    }

  } else if (method == "mining.authorize" && params.Size() >= 2) {
    Method = EAuthorize;
    if (params[0].IsString() && params[1].IsString()) {
      Authorize.login = params[0].GetString();
      Authorize.password = params[1].GetString();
    } else {
      return EStratumStatusFormatError;
    }
  } else if (method == "mining.extranonce.subscribe") {
    Method = EExtraNonceSubscribe;
  } else if (method == "mining.submit" && params.Size() >= 5) {
    return EStratumStatusFormatError;
  } else {
    return EStratumStatusFormatError;
  }

  return EStratumStatusOk;
}

void Stratum::WorkerConfig::onSubscribe(MiningConfig &miningCfg, StratumMessage &msg, xmstream &out, std::string &subscribeInfo)
{
  // Response format
  // {"id": 1, "result": [["mining.notify", "ae6812eb4cd7735a302a8a9dd95cf71f", "EthereumStratum/1.0.0"], "080c"],"error": null}

  {
    JSON::Object object(out);
    addId(object, msg);
    object.addField("result");
    {
      JSON::Array resultValue(out);
      resultValue.addField();
      {
        JSON::Array sessions(out);
        sessions.addField();
        {
          JSON::Array notifySession(out);
          notifySession.addString("mining.notify");
          notifySession.addString(NotifySession);
        }
      }

      // Unique extra nonce
      resultValue.addString(writeHexBE(ExtraNonceFixed, miningCfg.FixedExtraNonceSize));
    }
    object.addNull("error");
  }

  out.write('\n');
  subscribeInfo = std::to_string(ExtraNonceFixed);
}

bool Stratum::Work::loadFromTemplate(rapidjson::Value &document, const std::string &ticker, std::string &error)
{
  if (!document.HasMember("result") || !document["result"].IsArray()) {
    error = "no result";
    return false;
  }

  rapidjson::Value::Array resultValue = document["result"].GetArray();
  if (resultValue.Size() != 4 ||
      !resultValue[0].IsString() || resultValue[0].GetStringLength() != 66 ||
      !resultValue[1].IsString() || resultValue[1].GetStringLength() != 66 ||
      !resultValue[2].IsString() || resultValue[2].GetStringLength() != 66 ||
      !resultValue[3].IsString()) {
    error = "getWork format error";
    return false;
  }

  HeaderHash_ = resultValue[0].GetString() + 2;
  SeedHash_ = resultValue[1].GetString() + 2;
  this->Height_ = strtoul(resultValue[3].GetString()+2, nullptr, 16);
  // TODO: calculate block reward
  this->BlockReward_ = 0;
  return true;
}

void Stratum::Work::buildNotifyMessage(bool resetPreviousWork)
{
  {
    NotifyMessage_.reset();
    JSON::Object root(NotifyMessage_);
    root.addNull("id");
    root.addString("method", "mining.notify");
    root.addField("params");
    {
      JSON::Array params(NotifyMessage_);
      {
        // Id
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%" PRIi64 "#%u", this->StratumId_, this->SendCounter_++);
        params.addString(buffer);
      }
      // Seed hash
      params.addString(SeedHash_);
      // Header hash
      params.addString(HeaderHash_);

    }
  }
  NotifyMessage_.write('\n');
}
}
