#include "blockmaker/eth.h"
#include "blockmaker/ethereumBlockTemplate.h"

namespace ETH {
bool Stratum::Work::loadFromTemplate(CBlockTemplate &blockTemplate, std::string &error)
{
  CETHGetWorkResult &work = *static_cast<CEthereumBlockTemplate&>(blockTemplate).Data.Result;

  HeaderHash_ = work.Header_hash;
  std::reverse(HeaderHash_.begin(), HeaderHash_.end());
  HeaderHashHex_ = work.Header_hash.getHexLE();
  SeedHashHex_ = work.Seed_hash.getHexLE();

  if (work.Target.size() >= 3 && work.Target[0] == '0' && work.Target[1] == 'x')
    Target_.setHex(work.Target.c_str() + 2);

  if (work.Block_height.size() >= 3 && work.Block_height[0] == '0' && work.Block_height[1] == 'x')
    this->Height_ = strtoul(work.Block_height.c_str() + 2, nullptr, 16);

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
  status.PowHash = FinalHash_;
  status.IsShare = status.PowHash <= shareTarget;
  status.IsBlock = status.PowHash <= Target_;
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
