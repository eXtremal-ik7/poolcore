#include "poolcore/poolCore.h"

#include "poolcommon/bech32.h"
#include "poolcore/base58.h"
#include "openssl/sha.h"
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
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, &decodedAddress[0], decodedAddress.size() - 4);
  SHA256_Final(sha256, &ctx);

  SHA256_Init(&ctx);
  SHA256_Update(&ctx, sha256, sizeof(sha256));
  SHA256_Final(sha256, &ctx);
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

uint64_t CCoinInfo::calculateAveragePower(double work, uint64_t timeInterval, const std::vector<uint32_t> &primePOWShares) const
{
  switch (PowerUnitType) {
    case EHash :
      return static_cast<uint64_t>(work / timeInterval * (WorkMultiplier / pow(10.0, PowerMultLog10)));
    case ECPD : {
      // TODO: pass through arguments
      constexpr unsigned target = 10;
      return 1000.0 * (work / timeInterval * pow(0.1, target - 7) * 24*3600);
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
