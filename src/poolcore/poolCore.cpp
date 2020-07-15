#include "poolcore/poolCore.h"
#include "poolcore/base58.h"
#include "openssl/sha.h"
#include <string.h>
#include "loguru.hpp"

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

bool CCoinInfo::checkAddress(const std::string &address, EAddressType type)
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
