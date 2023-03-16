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
      std::vector<uint32_t> primePOWSum;
      size_t shareLengthCount = primePOWShares.size();
      primePOWSum.resize(shareLengthCount);
      uint32_t sum = 0;
      uint32_t minShare = shareLengthCount;
      for (size_t i = 0, ie = shareLengthCount; i != ie; ++i) {
        sum += primePOWShares[shareLengthCount - i - 1];
        primePOWSum[shareLengthCount - i - 1] = sum;
        if (primePOWShares[shareLengthCount - i - 1])
          minShare = shareLengthCount - i - 1;
      }

      // Check possibility of CPD calculation
      // TODO: pass real target
      unsigned target = 10;
      if (minShare+1 < shareLengthCount && minShare < target && primePOWSum[minShare] > 0 && primePOWSum[minShare+1] > 0) {
        unsigned targetDiff = target - minShare;
        double primeProb = static_cast<double>(primePOWSum[minShare + 1]) / static_cast<double>(primePOWSum[minShare]);
        // TODO: remove it
        fprintf(stderr, "Shares: ");
        for (unsigned i = 0; i < shareLengthCount; i++)
          fprintf(stderr, "%u, ", primePOWShares[i]);
        fprintf(stderr, "\n");

        fprintf(stderr, "Sum: ");
        for (unsigned i = 0; i < shareLengthCount; i++)
          fprintf(stderr, "%u, ", primePOWSum[i]);
        fprintf(stderr, "\n");

        fprintf(stderr, "minShares=%u\n", primePOWSum[minShare]);
        fprintf(stderr, "primeProb=%.4lf\n", primeProb);
        fprintf(stderr, "interval=%u\n", (unsigned)timeInterval);
        return static_cast<double>(primePOWSum[minShare]) / static_cast<double>(timeInterval) * 24.0 * 3600.0 * pow(primeProb, targetDiff) * 1000.0;
      } else {
        return 0;
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
