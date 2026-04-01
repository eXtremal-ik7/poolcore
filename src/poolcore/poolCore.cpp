#include "poolcore/poolCore.h"

#include "poolcommon/bech32.h"
#include "poolcommon/utils.h"
#include "poolcore/backendData.h"
#include "poolcore/base58.h"
#include "poolcore/blockTemplate.h"
#include "poolcore/poolInstance.h"
#include "blockmaker/sha256.h"
#include <string.h>
#include "loguru.hpp"
#include <math.h>

static bool checkBase58Address(const std::vector<uint8_t> &decodedAddress, const std::vector<uint8_t> &prefix)
{
  if (decodedAddress.size() != prefix.size() + 20 + 4)
    return false;

  if (memcmp(&decodedAddress[0], &prefix[0], prefix.size()) != 0) {
    return false;
  }

  uint32_t addrHash;
  memcpy(&addrHash, &decodedAddress[prefix.size() + 20], 4);

  uint8_t sha256[32];
  CCtxSha256 ctx;
  sha256Init(&ctx);
  sha256Update(&ctx, &decodedAddress[0], decodedAddress.size() - 4);
  sha256Final(&ctx, sha256);

  sha256Init(&ctx);
  sha256Update(&ctx, sha256, sizeof(sha256));
  sha256Final(&ctx, sha256);
  return reinterpret_cast<uint32_t*>(sha256)[0] == addrHash;
}

static bool checkBech32Address(const std::string &address, const std::string &prefix)
{
  auto result = bech32::Decode(address);
  return result.second.size() > 0 && result.first == prefix;
}

bool CCoinInfo::checkAddress(const std::string &address, EAddressType type) const
{
  if (type & (EP2PKH | EPS2H)) {
    std::vector<uint8_t> decoded;
    if (DecodeBase58(address, decoded)) {
      if ((type & EP2PKH) && checkBase58Address(decoded, PubkeyAddressPrefix)) {
        return true;
      } else if ((type & EPS2H) && checkBase58Address(decoded, ScriptAddressPrefix)) {
        return true;
      }
    }
  }

  if ((type & EBech32) && checkBech32Address(address, Bech32Prefix)) {
    return true;
  }

  if (type & EBCH) {
    auto addr = bech32::DecodeCashAddrContent(address, "bitcoincash");
    if (!addr.hash.empty())
      return true;
  }

  if (type & EECash) {
    auto addr = bech32::DecodeCashAddrContent(address, "ecash");
    if (!addr.hash.empty())
      return true;
  }

  if (type & EEth) {
    if (address.size() == 42 && address[0] == '0' && address[1] == 'x')
      return true;
  }

  return false;
}

const char *CCoinInfo::getPowerUnitName() const
{
  switch (PowerUnitType) {
    case EHash : return "hash";
    case ECPD : return "cpd";
  }

  return "???";
}

uint64_t CCoinInfo::calculateAveragePower(const UInt<256> &work, uint64_t timeInterval, unsigned int primePOWTarget) const
{
  switch (PowerUnitType) {
    case EHash : {
      UInt<256> power = work / timeInterval;
      power /= static_cast<uint64_t>(pow(10.0, PowerMultLog10));
      return power.low64();
    }
    case ECPD : {
      if (primePOWTarget == -1U) {
        return 0;
      } else {
        UInt<256> power = (work * 1000ull * 24ull * 3600ull) / timeInterval;
        power.mulfp(pow(0.1, primePOWTarget - 7));
        power >>= 32;
        return power.low64();
      }
    }
  }

  return 0;
}


void CNetworkClient::CSubmitBlockOperation::accept(bool result, const std::string &hostName, const std::string &error)
{
  uint32_t st = 1u + ((result ? 1u : 0) << 16);
  uint32_t currentState = State_.fetch_add(st) + st;
  uint32_t totalSubmits = currentState & 0xFFFF;
  uint32_t successSubmits = currentState >> 16;
  Callback_(result, successSubmits, hostName, error);
  if (totalSubmits == ClientsNum_)
    delete this;
}

void CNetworkClient::aioSubmitBlock(asyncBase *base, const void *data, size_t size, SumbitBlockCb callback)
{
  aioSubmitBlock(base, data, size, new CSubmitBlockOperation(std::move(callback), getWorkClientsNum()));
}

// --- CNetworkClient multi-node infrastructure ---

void CNetworkClient::connectWith(CPoolInstance *instance)
{
  LinkedInstances_.push_back(instance);
}

void CNetworkClient::start(asyncBase *base, const CCoinInfo &coinInfo)
{
  Base_ = base;
  CoinInfo_ = coinInfo;

  ReconnectTimer_ = newUserEvent(base, 0, [](aioUserEvent *, void *arg) {
    static_cast<CNetworkClient*>(arg)->onReconnectTimer();
  }, this);

  Estimator_ = std::make_unique<CFeeEstimator>();
  FeeUpdateTimer_ = std::make_unique<CPeriodicTimer>(base);
  FeeUpdateTimer_->start([this]() {
    if (PendingHeight_ == 0)
      return;
    updateFeeEstimation();
  });
}

void CNetworkClient::stop()
{
  Stopped_ = true;
  if (ReconnectTimer_)
    userEventStartTimer(ReconnectTimer_, 0, 0);
  if (FeeUpdateTimer_) {
    FeeUpdateTimer_->stop();
    FeeUpdateTimer_->wait(CoinInfo_.Name.c_str(), "fee estimation");
    FeeUpdateTimer_.reset();
  }
  Estimator_.reset();
}

void CNetworkClient::reportNewWork(CBlockTemplate *blockTemplate)
{
  if (Stopped_)
    return;

  WorkState_ = EWorkOk;
  intrusive_ptr<CBlockTemplate> holder(blockTemplate);
  for (auto &instance : LinkedInstances_)
    instance->checkNewBlockTemplate(blockTemplate, Backend_);

  if (NetworkState_)
    NetworkState_->store({blockTemplate->Height, blockTemplate->Difficulty});

  int64_t height = blockTemplate->Height;
  if (height > PendingHeight_) {
    PendingHeight_ = height;
    if (FeeUpdateTimer_)
      FeeUpdateTimer_->activate();
  }
}

void CNetworkClient::reportConnectionLost()
{
  if (Stopped_)
    return;

  if (WorkState_ == EWorkOk) {
    WorkState_ = EWorkLost;
    ConnectionLostTime_ = std::chrono::steady_clock::now();
  }

  userEventStartTimer(ReconnectTimer_, 100000, 1);
}

void CNetworkClient::onReconnectTimer()
{
  if (Stopped_)
    return;

  if (WorkState_ == EWorkLost) {
    auto now = std::chrono::steady_clock::now();
    int64_t noWorkTime = std::chrono::duration_cast<std::chrono::seconds>(now - ConnectionLostTime_).count();
    if (noWorkTime >= 5) {
      for (auto &instance : LinkedInstances_)
        instance->stopWork();
      WorkState_ = EWorkMinersStopped;
    }
  }

  doAdvanceWorkFetcher();
}
