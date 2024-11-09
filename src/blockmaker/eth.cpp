#include "blockmaker/eth.h"
#include "poolcommon/arith_uint256.h"

static inline double getDifficulty(uint32_t bits)
{
    int nShift = (bits >> 24) & 0xff;
    double dDiff =
        (double)0x0000ffff / (double)(bits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

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
  } else if (method == "mining.submit" && params.Size() == 3) {
    Method = ESubmit;
    Submit.WorkerName = params[0].GetString();
    Submit.JobId = params[1].GetString();
    Submit.Nonce = strtoul(params[2].GetString(), nullptr, 16);
  } else if (method == "eth_submitHashrate") {
    Method = ESubmitHashrate;
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
        JSON::Array notifySession(out);
        notifySession.addString("mining.notify");
        notifySession.addString(NotifySession);
        notifySession.addString("EthereumStratum/1.0.0");
      }
      // Unique extra nonce
      resultValue.addString(writeHexBE(ExtraNonceFixed, miningCfg.FixedExtraNonceSize));
    }
    object.addNull("error");
  }

  out.write('\n');
  subscribeInfo = std::to_string(ExtraNonceFixed);
}

bool Stratum::Work::loadFromTemplate(CBlockTemplate &blockTemplate, const std::string &ticker, std::string &error)
{
  if (!blockTemplate.Document.HasMember("result") || !blockTemplate.Document["result"].IsArray()) {
    error = "no result";
    return false;
  }

  rapidjson::Value::Array resultValue = blockTemplate.Document["result"].GetArray();
  if (resultValue.Size() != 4 ||
      !resultValue[0].IsString() || resultValue[0].GetStringLength() != 66 ||
      !resultValue[1].IsString() || resultValue[1].GetStringLength() != 66 ||
      !resultValue[2].IsString() || resultValue[2].GetStringLength() != 66 ||
      !resultValue[3].IsString()) {
    error = "getWork format error";
    return false;
  }

  HeaderHashHex_ = resultValue[0].GetString() + 2;
  SeedHashHex_ = resultValue[1].GetString() + 2;
  HeaderHash_.SetHex(HeaderHashHex_);
  std::reverse(HeaderHash_.begin(), HeaderHash_.end());
  Target_.SetHex(resultValue[2].GetString() + 2);
  this->Height_ = strtoul(resultValue[3].GetString()+2, nullptr, 16);
  // Block reward can't be calculated at this moment
  this->BlockReward_ = 0;

  // Copy reference to DAG file & check it
  DagFile_ = blockTemplate.DagFile;
  if (DagFile_.get() == nullptr) {
    error = "DAG file is empty";
    return false;
  }

  return true;
}

bool Stratum::Work::prepareForSubmit(const WorkerConfig &workerCfg, const StratumMessage &msg)
{
  Nonce_ = (workerCfg.ExtraNonceFixed << (64 - 8*MiningCfg_.FixedExtraNonceSize)) | msg.Submit.Nonce;
  ethashCalculate(FinalHash_.begin(), MixHash_.begin(), HeaderHash_.begin(), Nonce_, DagFile_.get()->dag());
  std::reverse(FinalHash_.begin(), FinalHash_.begin()+32);
  return true;
}

void Stratum::Work::buildBlock(size_t, xmstream &blockHexData)
{
  blockHexData.reset();
  BlockSubmitData *data = blockHexData.reserve<ETH::BlockSubmitData>(1);
  // Nonce
  std::string nonce = writeHexBE(Nonce_, 8);
  data->Nonce[0] = '0';
  data->Nonce[1] = 'x';
  memcpy(&data->Nonce[2], nonce.data(), 16);
  data->Nonce[16+2] = 0;
  // Header hash
  data->HeaderHash[0] = '0';
  data->HeaderHash[1] = 'x';
  memcpy(&data->HeaderHash[2], HeaderHashHex_.data(), 64);
  data->HeaderHash[64+2] = 0;
  // Mix hash
  data->MixHash[0] = '0';
  data->MixHash[1] = 'x';
  bin2hexLowerCase(MixHash_.begin(), &data->MixHash[2], 32);
  data->MixHash[64+2] = 0;
}

CCheckStatus Stratum::Work::checkConsensus(size_t)
{
  CCheckStatus status;
  // Get difficulty
  status.ShareDiff = getDifficulty(FinalHash_.GetCompact());
  status.IsBlock = FinalHash_ <= Target_;
  status.IsPendingBlock = false;
  return status;
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
      params.addString(SeedHashHex_);
      // Header hash
      params.addString(HeaderHashHex_);

    }
  }
  NotifyMessage_.write('\n');
}
}
