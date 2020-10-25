#include "poolcore/poolCore.h"
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

bool CCoinInfo::checkAddress(const std::string &address, EAddressType type) const
{
  if (type & (EP2PKH | EPS2H | EBech32)) {
    std::vector<uint8_t> decoded;
    if (!DecodeBase58(address, decoded))
      return false;

    if ((type & EP2PKH) && checkBase58Address(decoded, PubkeyAddressPrefix)) {
      return true;
    } else if ((type & EPS2H) && checkBase58Address(decoded, ScriptAddressPrefix)) {
      return true;
    } else if ((type & EBech32)) {
      LOG_F(WARNING, "bech32 addresses not supported now");
    }
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

uint64_t CCoinInfo::calculateAveragePower(double work, uint64_t timeInterval) const
{
  switch (PowerUnitType) {
    case EHash : {
      static double workMultiplier = 4294967296.0;
      return static_cast<uint64_t>(work / timeInterval * (workMultiplier / pow(10.0, PowerMultLog10)));
    }

    case ECPD :
      // TODO: implement
      return 0;
  }

  return 0;
}


void CNetworkClient::CSubmitBlockOperation::accept(bool result, const std::string &hostName, const std::string &error)
{
  uint32_t st = 1u + ((result ? 1u : 0) << 16);
  uint32_t currentState = State_.fetch_add(st) + st;
  uint32_t successSubmits = currentState >> 16;
  Callback_(successSubmits, hostName, error);
  if ((st & 0xFFFF) == ClientsNum_)
    delete this;
}
