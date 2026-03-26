// Copyright (c) 2017 Pieter Wuille
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Bech32 is a string encoding format used in newer address types.
// The output consists of a human-readable part (alphanumeric), a
// separator character (1), and a base32 data section, the last
// 6 characters of which are a checksum.
//
// For more information, see BIP 173.

#include <stdint.h>
#include <string>
#include <vector>

template <int frombits, int tobits, bool pad, typename O, typename I>
bool ConvertBits(const O &outfn, I it, I end) {
    size_t acc = 0;
    size_t bits = 0;
    constexpr size_t maxv = (1 << tobits) - 1;
    constexpr size_t max_acc = (1 << (frombits + tobits - 1)) - 1;
    while (it != end) {
        acc = ((acc << frombits) | *it) & max_acc;
        bits += frombits;
        while (bits >= tobits) {
            bits -= tobits;
            outfn((acc >> bits) & maxv);
        }
        ++it;
    }
    if (pad) {
        if (bits) {
            outfn((acc << (tobits - bits)) & maxv);
        }
    } else if (bits >= frombits || ((acc << (tobits - bits)) & maxv)) {
        return false;
    }
    return true;
}

namespace bech32 {

enum CashAddrType : uint8_t {
  PUBKEY_TYPE = 0,
  SCRIPT_TYPE = 1
};

struct CashAddrContent {
    CashAddrType type;
    std::vector<uint8_t> hash;
};

/** Encode a Bech32 string. If hrp contains uppercase characters, this will cause an assertion error. */
std::string Encode(const std::string& hrp, const std::vector<uint8_t>& values);

/** Decode a Bech32 string. Returns (hrp, data). Empty hrp means failure. */
std::pair<std::string, std::vector<uint8_t>> Decode(const std::string& str);
/** Encode a CashAddr string (used by BCH, XEC). */
std::string EncodeCashAddr(const std::string &prefix, CashAddrType type, const std::vector<uint8_t> &hash);

std::pair<std::string, std::vector<uint8_t>> DecodeCashAddr(const std::string &str, const std::string &default_prefix);
CashAddrContent DecodeCashAddrContent(const std::string &addr, const std::string &expectedPrefix);

} // namespace bech32 
