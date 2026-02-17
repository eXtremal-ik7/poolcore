#include "blockmaker/eth.h"

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
bool Stratum::Work::loadFromTemplate(CBlockTemplate &blockTemplate, std::string &error)
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
  HeaderHash_.setHexLE(HeaderHashHex_.c_str());
  std::reverse(HeaderHash_.begin(), HeaderHash_.end());
  Target_.setHex(resultValue[2].GetString() + 2);
  this->Height_ = strtoul(resultValue[3].GetString()+2, nullptr, 16);
  // Block reward can't be calculated at this moment
  this->BlockReward_ = UInt<384>::zero();
  this->BaseBlockReward_ = ETH::getConstBlockReward(blockTemplate.Ticker, this->Height_);

  // Copy reference to DAG file & check it
  DagFile_ = blockTemplate.DagFile;
  if (DagFile_.get() == nullptr) {
    error = "DAG file is empty";
    return false;
  }

  return true;
}

bool Stratum::Work::prepareForSubmit(const CWorkerConfig &workerCfg, const CStratumMessage &msg)
{
  Nonce_ = (workerCfg.ExtraNonceFixed << (64 - 8*MiningCfg_.FixedExtraNonceSize)) | msg.Submit.ETH.Nonce;
  ethashCalculate(FinalHash_.rawData(), MixHash_.begin(), HeaderHash_.begin(), Nonce_, DagFile_.get()->dag());
  std::reverse(FinalHash_.rawData(), FinalHash_.rawData() + FinalHash_.rawSize());
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

CCheckStatus Stratum::Work::checkConsensus(size_t, const UInt<256> &shareTarget)
{
  CCheckStatus status;
  status.IsShare = FinalHash_ <= shareTarget;
  status.IsBlock = FinalHash_ <= Target_;
  status.IsPendingBlock = false;
  return status;
}

void Stratum::Work::buildNotifyMessage(bool)
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
