#include <stdio.h>
#include <string.h>
#include <getopt.h>

#include "poolcore/coinLibrary.h"
#include "poolcore/base58.h"
#include "poolcommon/bech32.h"
#include "poolcommon/utils.h"
#include "blockmaker/sha256.h"
#include "blockmaker/sha3.h"

#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <secp256k1.h>

#include "poolcore/plugin.h"

CPluginContext gPluginContext;

enum CmdLineOptsTy {
  clOptHelp = 1,
  clOptCoin,
  clOptType
};

static option cmdLineOpts[] = {
  {"help", no_argument, nullptr, clOptHelp},
  {"coin", required_argument, nullptr, clOptCoin},
  {"type", required_argument, nullptr, clOptType},
  {nullptr, 0, nullptr, 0}
};

static std::string encodeBase58Check(const uint8_t *data, size_t size)
{
  uint8_t sha256[32];
  CCtxSha256 ctx;
  sha256Init(&ctx);
  sha256Update(&ctx, data, size);
  sha256Final(&ctx, sha256);

  sha256Init(&ctx);
  sha256Update(&ctx, sha256, sizeof(sha256));
  sha256Final(&ctx, sha256);

  std::vector<uint8_t> result(data, data + size);
  result.insert(result.end(), sha256, sha256 + 4);
  return EncodeBase58(result);
}

static std::string encodeWIF(uint8_t prefix, const uint8_t *privateKey, bool compressed)
{
  // WIF: prefix(1) + key(32) + [compressed_flag(1)] + checksum(4)
  uint8_t data[34];
  data[0] = prefix;
  memcpy(data + 1, privateKey, 32);
  size_t dataSize = 33;
  if (compressed) {
    data[33] = 0x01;
    dataSize = 34;
  }
  return encodeBase58Check(data, dataSize);
}

static std::string encodeP2PKHAddress(const std::vector<uint8_t> &prefix, const uint8_t *pubkeyHash)
{
  std::vector<uint8_t> data;
  data.insert(data.end(), prefix.begin(), prefix.end());
  data.insert(data.end(), pubkeyHash, pubkeyHash + 20);
  return encodeBase58Check(data.data(), data.size());
}

static std::string encodeP2SHAddress(const std::vector<uint8_t> &prefix, const uint8_t *pubkeyHash)
{
  // Legacy P2SH: redeem script = OP_DUP OP_HASH160 <push20> <pubkeyHash> OP_EQUALVERIFY OP_CHECKSIG
  uint8_t redeemScript[25];
  redeemScript[0] = 0x76; // OP_DUP
  redeemScript[1] = 0xa9; // OP_HASH160
  redeemScript[2] = 0x14; // push 20 bytes
  memcpy(redeemScript + 3, pubkeyHash, 20);
  redeemScript[23] = 0x88; // OP_EQUALVERIFY
  redeemScript[24] = 0xac; // OP_CHECKSIG

  // Hash160 the redeem script: SHA256 + RIPEMD160
  uint8_t sha256[32];
  CCtxSha256 ctx;
  sha256Init(&ctx);
  sha256Update(&ctx, redeemScript, sizeof(redeemScript));
  sha256Final(&ctx, sha256);

  uint8_t scriptHash[20];
  EVP_MD_CTX *ripemdCtx = EVP_MD_CTX_create();
  EVP_DigestInit(ripemdCtx, EVP_ripemd160());
  EVP_DigestUpdate(ripemdCtx, sha256, sizeof(sha256));
  EVP_DigestFinal(ripemdCtx, scriptHash, nullptr);
  EVP_MD_CTX_destroy(ripemdCtx);

  std::vector<uint8_t> data;
  data.insert(data.end(), prefix.begin(), prefix.end());
  data.insert(data.end(), scriptHash, scriptHash + 20);
  return encodeBase58Check(data.data(), data.size());
}

static std::string encodeBech32Address(const std::string &hrp, const uint8_t *pubkeyHash)
{
  std::vector<uint8_t> data;
  data.push_back(0); // witness version 0
  const uint8_t *begin = pubkeyHash;
  const uint8_t *end = pubkeyHash + 20;
  ConvertBits<8, 5, true>([&](uint8_t c) { data.push_back(c); }, begin, end);
  return bech32::Encode(hrp, data);
}

static std::string encodeEthAddress(const uint8_t *addressBytes)
{
  // EIP-55 checksum encoding
  std::string hexAddr = bin2hexLowerCase(addressBytes, 20);

  uint8_t addrHash[32];
  sha3(hexAddr.data(), 40, addrHash, 32, 1);

  std::string result = "0x";
  for (int i = 0; i < 40; i++) {
    uint8_t hashNibble = (addrHash[i / 2] >> (i % 2 == 0 ? 4 : 0)) & 0x0f;
    if (hexAddr[i] >= 'a' && hexAddr[i] <= 'f' && hashNibble >= 8)
      result += (char)(hexAddr[i] - 32); // uppercase
    else
      result += hexAddr[i];
  }
  return result;
}

static void printUsage()
{
  fprintf(stderr,
    "Usage: addressgen --coin <COIN> [--type <p2pkh|p2sh|bech32|cashaddr|eth>]\n"
    "\n"
    "Options:\n"
    "  --coin <COIN>    Coin ticker (e.g. BTC, LTC, DOGE, ETC, BCHN, XEC)\n"
    "  --type <TYPE>    Address type: p2pkh (default for UTXO coins),\n"
    "                   p2sh, bech32, cashaddr (BCH/XEC), eth (default for account-based)\n"
    "  --help           Show this help\n"
  );
}

int main(int argc, char **argv)
{
  const char *coinName = nullptr;
  const char *addressType = nullptr;

  int res;
  while ((res = getopt_long_only(argc, argv, "", cmdLineOpts, nullptr)) != -1) {
    switch (res) {
      case clOptHelp:
        printUsage();
        return 0;
      case clOptCoin:
        coinName = optarg;
        break;
      case clOptType:
        addressType = optarg;
        break;
      default:
        break;
    }
  }

  if (!coinName) {
    fprintf(stderr, "Error: --coin is required\n");
    printUsage();
    return 1;
  }

  // Get coin info
  CCoinInfo coinInfo = CCoinLibrary::get(coinName);
  if (coinInfo.Name.empty()) {
    for (const auto &proc : gPluginContext.AddExtraCoinProcs) {
      if (proc(coinName, coinInfo))
        break;
    }
  }
  if (coinInfo.Name.empty()) {
    fprintf(stderr, "Error: unknown coin '%s'\n", coinName);
    return 1;
  }

  bool isAccountBased = (coinInfo.PayoutAddressType & CCoinInfo::EEth) != 0;
  bool isCashAddr = (coinInfo.PayoutAddressType & (CCoinInfo::EBCH | CCoinInfo::EECash)) != 0;

  // Default address type based on coin
  if (!addressType) {
    if (isAccountBased)
      addressType = "eth";
    else if (isCashAddr)
      addressType = "cashaddr";
    else
      addressType = "p2pkh";
  }

  // Determine address type
  enum { TypeP2PKH, TypeP2SH, TypeBech32, TypeCashAddr, TypeEth } type;
  if (strcmp(addressType, "p2pkh") == 0) {
    type = TypeP2PKH;
  } else if (strcmp(addressType, "p2sh") == 0) {
    type = TypeP2SH;
  } else if (strcmp(addressType, "bech32") == 0) {
    type = TypeBech32;
  } else if (strcmp(addressType, "cashaddr") == 0) {
    type = TypeCashAddr;
  } else if (strcmp(addressType, "eth") == 0) {
    type = TypeEth;
  } else {
    fprintf(stderr, "Error: unknown address type '%s' (use p2pkh, p2sh, bech32, cashaddr, eth)\n", addressType);
    return 1;
  }

  // Validate address type is supported
  if (type == TypeEth && !isAccountBased) {
    fprintf(stderr, "Error: %s is not an account-based coin, use p2pkh/p2sh/bech32/cashaddr\n", coinName);
    return 1;
  }
  if (type != TypeEth && isAccountBased) {
    fprintf(stderr, "Error: %s is account-based, only eth address type is supported\n", coinName);
    return 1;
  }
  if (type == TypeP2PKH && !(coinInfo.PayoutAddressType & CCoinInfo::EP2PKH)) {
    fprintf(stderr, "Error: %s does not support P2PKH addresses\n", coinName);
    return 1;
  }
  if (type == TypeP2SH && !(coinInfo.PayoutAddressType & CCoinInfo::EPS2H)) {
    fprintf(stderr, "Error: %s does not support P2SH addresses\n", coinName);
    return 1;
  }
  if (type == TypeBech32 && !(coinInfo.PayoutAddressType & CCoinInfo::EBech32)) {
    fprintf(stderr, "Error: %s does not support Bech32 addresses\n", coinName);
    return 1;
  }
  if (type == TypeCashAddr && !isCashAddr) {
    fprintf(stderr, "Error: %s does not support CashAddr addresses\n", coinName);
    return 1;
  }
  if (type != TypeEth && coinInfo.PrivateKeyPrefix == 0) {
    fprintf(stderr, "Error: %s has no WIF private key prefix configured\n", coinName);
    return 1;
  }

  // Generate private key and derive public key
  secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
  uint8_t privateKey[32];
  secp256k1_pubkey pubkey;
  for (;;) {
    if (RAND_priv_bytes(privateKey, sizeof(privateKey)) != 1) {
      fprintf(stderr, "Error: RAND_priv_bytes failed\n");
      secp256k1_context_destroy(ctx);
      return 1;
    }
    if (secp256k1_ec_pubkey_create(ctx, &pubkey, privateKey))
      break;
  }

  std::string address;
  std::string privateKeyEncoded;
  std::string redeemScriptHex;

  if (type == TypeEth) {
    // Ethereum: uncompressed pubkey (65 bytes, skip first 0x04 byte) -> Keccak-256 -> last 20 bytes
    uint8_t uncompressedPubkey[65];
    size_t pubkeySize = sizeof(uncompressedPubkey);
    secp256k1_ec_pubkey_serialize(ctx, uncompressedPubkey, &pubkeySize, &pubkey, SECP256K1_EC_UNCOMPRESSED);

    uint8_t keccakHash[32];
    sha3(uncompressedPubkey + 1, 64, keccakHash, 32, 1);

    address = encodeEthAddress(keccakHash + 12);
    privateKeyEncoded = "0x" + bin2hexLowerCase(privateKey, 32);
  } else {
    // Bitcoin-like: compressed pubkey -> SHA256 -> RIPEMD160
    uint8_t compressedPubkey[33];
    size_t pubkeySize = sizeof(compressedPubkey);
    secp256k1_ec_pubkey_serialize(ctx, compressedPubkey, &pubkeySize, &pubkey, SECP256K1_EC_COMPRESSED);

    uint8_t sha256Hash[32];
    CCtxSha256 sha256Ctx;
    sha256Init(&sha256Ctx);
    sha256Update(&sha256Ctx, compressedPubkey, pubkeySize);
    sha256Final(&sha256Ctx, sha256Hash);

    uint8_t pubkeyHash[20];
    EVP_MD_CTX *ripemdCtx = EVP_MD_CTX_create();
    EVP_DigestInit(ripemdCtx, EVP_ripemd160());
    EVP_DigestUpdate(ripemdCtx, sha256Hash, sizeof(sha256Hash));
    EVP_DigestFinal(ripemdCtx, pubkeyHash, nullptr);
    EVP_MD_CTX_destroy(ripemdCtx);

    switch (type) {
      case TypeP2PKH:
        address = encodeP2PKHAddress(coinInfo.PubkeyAddressPrefix, pubkeyHash);
        break;
      case TypeP2SH: {
        uint8_t redeemScript[25];
        redeemScript[0] = 0x76; // OP_DUP
        redeemScript[1] = 0xa9; // OP_HASH160
        redeemScript[2] = 0x14; // push 20 bytes
        memcpy(redeemScript + 3, pubkeyHash, 20);
        redeemScript[23] = 0x88; // OP_EQUALVERIFY
        redeemScript[24] = 0xac; // OP_CHECKSIG
        redeemScriptHex = bin2hexLowerCase(redeemScript, sizeof(redeemScript));
        address = encodeP2SHAddress(coinInfo.ScriptAddressPrefix, pubkeyHash);
        break;
      }
      case TypeBech32:
        address = encodeBech32Address(coinInfo.Bech32Prefix, pubkeyHash);
        break;
      case TypeCashAddr: {
        std::string cashPrefix = (coinInfo.PayoutAddressType & CCoinInfo::EECash) ? "ecash" : "bitcoincash";
        std::vector<uint8_t> hash(pubkeyHash, pubkeyHash + 20);
        address = bech32::EncodeCashAddr(cashPrefix, bech32::PUBKEY_TYPE, hash);
        break;
      }
      default:
        break;
    }

    privateKeyEncoded = encodeWIF(coinInfo.PrivateKeyPrefix, privateKey, true);
  }

  secp256k1_context_destroy(ctx);

  // Output
  fprintf(stdout, "coin: %s (%s)\n", coinInfo.Name.c_str(), coinInfo.FullName.c_str());
  fprintf(stdout, "type: %s\n", addressType);
  fprintf(stdout, "address: %s\n", address.c_str());
  if (type == TypeEth)
    fprintf(stdout, "private key: %s\n", privateKeyEncoded.c_str());
  else
    fprintf(stdout, "private key (WIF): %s\n", privateKeyEncoded.c_str());
  if (type == TypeP2SH)
    fprintf(stdout, "redeem script: %s\n", redeemScriptHex.c_str());
  if (type == TypeBech32)
    fprintf(stdout, "descriptor: wpkh(%s)\n", privateKeyEncoded.c_str());
  fprintf(stdout, "Note: private key generated by OpenSSL %s using RAND_priv_bytes\n", OpenSSL_version(OPENSSL_VERSION));

  return 0;
}
