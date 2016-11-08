#include "poolcore/base58.h"
#include "poolcore/uint256.h"
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <string.h>


/** All alphanumeric characters except for "0", "I", "O", and "l" */
static const char* pszBase58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

enum Base58Type {
  PUBKEY_ADDRESS,
  SCRIPT_ADDRESS,
  SECRET_KEY,
  EXT_PUBLIC_KEY,
  EXT_SECRET_KEY,

  ZCPAYMENT_ADDRRESS,
  ZCSPENDING_KEY,

  MAX_BASE58_TYPES
};

/** Compute the 256-bit hash of an object. */
template<typename T1>
inline uint256 Hash(const T1 pbegin, const T1 pend)
{
  SHA256_CTX ctx;  
  static const unsigned char pblank[1] = {};
  uint256 result;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, pbegin == pend ? pblank : (const unsigned char*)&pbegin[0], (pend - pbegin) * sizeof(pbegin[0]));
  SHA256_Final((unsigned char*)&result, &ctx);
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, result.begin(), 32);
  SHA256_Final((unsigned char*)&result, &ctx);
  return result;
}

bool DecodeBase58(const char* psz, std::vector<unsigned char>& vch)
{
    // Skip leading spaces.
    while (*psz && isspace(*psz))
        psz++;
    // Skip and count leading '1's.
    int zeroes = 0;
    while (*psz == '1') {
        zeroes++;
        psz++;
    }
    // Allocate enough space in big-endian base256 representation.
    std::vector<unsigned char> b256(strlen(psz) * 733 / 1000 + 1); // log(58) / log(256), rounded up.
    // Process the characters.
    while (*psz && !isspace(*psz)) {
        // Decode base58 character
        const char* ch = strchr(pszBase58, *psz);
        if (ch == NULL)
            return false;
        // Apply "b256 = b256 * 58 + ch".
        int carry = ch - pszBase58;
        for (std::vector<unsigned char>::reverse_iterator it = b256.rbegin(); it != b256.rend(); it++) {
            carry += 58 * (*it);
            *it = carry % 256;
            carry /= 256;
        }
        psz++;
    }
    // Skip trailing spaces.
    while (isspace(*psz))
        psz++;
    if (*psz != 0)
        return false;
    // Skip leading zeroes in b256.
    std::vector<unsigned char>::iterator it = b256.begin();
    while (it != b256.end() && *it == 0)
        it++;
    // Copy result into output vector.
    vch.reserve(zeroes + (b256.end() - it));
    vch.assign(zeroes, 0x00);
    while (it != b256.end())
        vch.push_back(*(it++));
    return true;
}

std::string EncodeBase58(const unsigned char* pbegin, const unsigned char* pend)
{
    // Skip & count leading zeroes.
    int zeroes = 0;
    while (pbegin != pend && *pbegin == 0) {
        pbegin++;
        zeroes++;
    }
    // Allocate enough space in big-endian base58 representation.
    std::vector<unsigned char> b58((pend - pbegin) * 138 / 100 + 1); // log(256) / log(58), rounded up.
    // Process the bytes.
    while (pbegin != pend) {
        int carry = *pbegin;
        // Apply "b58 = b58 * 256 + ch".
        for (std::vector<unsigned char>::reverse_iterator it = b58.rbegin(); it != b58.rend(); it++) {
            carry += 256 * (*it);
            *it = carry % 58;
            carry /= 58;
        }
        pbegin++;
    }
    // Skip leading zeroes in base58 result.
    std::vector<unsigned char>::iterator it = b58.begin();
    while (it != b58.end() && *it == 0)
        it++;
    // Translate the result into a string.
    std::string str;
    str.reserve(zeroes + (b58.end() - it));
    str.assign(zeroes, '1');
    while (it != b58.end())
        str += pszBase58[*(it++)];
    return str;
}

std::string EncodeBase58(const std::vector<unsigned char>& vch)
{
    return EncodeBase58(&vch[0], &vch[0] + vch.size());
}

bool DecodeBase58(const std::string& str, std::vector<unsigned char>& vchRet)
{
    return DecodeBase58(str.c_str(), vchRet);
}

std::string EncodeBase58Check(const std::vector<unsigned char>& vchIn)
{
    // add 4-byte hash check to the end
    std::vector<unsigned char> vch(vchIn);
    uint256 hash = Hash(vch.begin(), vch.end());
    vch.insert(vch.end(), (unsigned char*)&hash, (unsigned char*)&hash + 4);
    return EncodeBase58(vch);
}

bool DecodeBase58Check(const char* psz, std::vector<unsigned char>& vchRet)
{
    if (!DecodeBase58(psz, vchRet) ||
        (vchRet.size() < 4)) {
        vchRet.clear();
        return false;
    }
    // re-calculate the checksum, insure it matches the included 4-byte checksum
    uint256 hash = Hash(vchRet.begin(), vchRet.end() - 4);
    if (memcmp(&hash, &vchRet.end()[-4], 4) != 0) {
        vchRet.clear();
        return false;
    }
    vchRet.resize(vchRet.size() - 4);
    return true;
}

bool DecodeBase58Check(const std::string& str, std::vector<unsigned char>& vchRet)
{
    return DecodeBase58Check(str.c_str(), vchRet);
}

CBase58Data::CBase58Data()
{
    vchVersion.clear();
    vchData.clear();
}

void CBase58Data::SetData(const std::vector<unsigned char>& vchVersionIn, const void* pdata, size_t nSize)
{
    vchVersion = vchVersionIn;
    vchData.resize(nSize);
    if (!vchData.empty())
        memcpy(&vchData[0], pdata, nSize);
}

void CBase58Data::SetData(const std::vector<unsigned char>& vchVersionIn, const unsigned char* pbegin, const unsigned char* pend)
{
    SetData(vchVersionIn, (void*)pbegin, pend - pbegin);
}

bool CBase58Data::SetString(const char* psz, unsigned int nVersionBytes)
{
    std::vector<unsigned char> vchTemp;
    bool rc58 = DecodeBase58Check(psz, vchTemp);
    if ((!rc58) || (vchTemp.size() < nVersionBytes)) {
        vchData.clear();
        vchVersion.clear();
        return false;
    }
    vchVersion.assign(vchTemp.begin(), vchTemp.begin() + nVersionBytes);
    vchData.resize(vchTemp.size() - nVersionBytes);
    if (!vchData.empty())
        memcpy(&vchData[0], &vchTemp[nVersionBytes], vchData.size());
    OPENSSL_cleanse(&vchTemp[0], vchData.size());
    return true;
}

bool CBase58Data::SetString(const std::string& str, unsigned int nVersionBytes)
{
    return SetString(str.c_str(), nVersionBytes);
}

std::string CBase58Data::ToString() const
{
    std::vector<unsigned char> vch = vchVersion;
    vch.insert(vch.end(), vchData.begin(), vchData.end());
    return EncodeBase58Check(vch);
}

int CBase58Data::CompareTo(const CBase58Data& b58) const
{
    if (vchVersion < b58.vchVersion)
        return -1;
    if (vchVersion > b58.vchVersion)
        return 1;
    if (vchData < b58.vchData)
        return -1;
    if (vchData > b58.vchData)
        return 1;
    return 0;
}

bool CBitcoinAddress::SetString(const char* pszAddress)
{
    return CBase58Data::SetString(pszAddress, 2);
}

bool CBitcoinAddress::SetString(const std::string& strAddress)
{
    return SetString(strAddress.c_str());
}

bool CBitcoinAddress::IsValidForZCash() const
{
  std::vector<unsigned char> base58Prefixes[MAX_BASE58_TYPES];
  // guarantees the first 2 characters, when base58 encoded, are "t1"
  base58Prefixes[PUBKEY_ADDRESS]     = {0x1C,0xB8};
  // guarantees the first 2 characters, when base58 encoded, are "t3"
  base58Prefixes[SCRIPT_ADDRESS]     = {0x1C,0xBD};
  // the first character, when base58 encoded, is "5" or "K" or "L" (as in Bitcoin)
  base58Prefixes[SECRET_KEY]         = {0x80};
  // do not rely on these BIP32 prefixes; they are not specified and may change
  base58Prefixes[EXT_PUBLIC_KEY]     = {0x04,0x88,0xB2,0x1E};
  base58Prefixes[EXT_SECRET_KEY]     = {0x04,0x88,0xAD,0xE4};
  // guarantees the first 2 characters, when base58 encoded, are "zc"
  base58Prefixes[ZCPAYMENT_ADDRRESS] = {0x16,0x9A};
  // guarantees the first 2 characters, when base58 encoded, are "SK"
  base58Prefixes[ZCSPENDING_KEY]     = {0xAB,0x36};  
  
  bool fCorrectSize = vchData.size() == 20;
  bool fKnownVersion = vchVersion == base58Prefixes[PUBKEY_ADDRESS] ||
                         vchVersion == base58Prefixes[SCRIPT_ADDRESS];
  return fCorrectSize && fKnownVersion;
}
