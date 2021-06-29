// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UINT256_H
#define BITCOIN_UINT256_H

#include <assert.h>
#include <cstring>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <vector>

/** Template base class for fixed-sized opaque blobs. */
template<unsigned int BITS>
class base_blob
{
protected:
    static constexpr int WIDTH = BITS / 8;
    uint8_t data[WIDTH];
public:
    base_blob()
    {
        memset(data, 0, sizeof(data));
    }

    explicit base_blob(const std::vector<unsigned char>& vch);

    bool IsNull() const
    {
        for (int i = 0; i < WIDTH; i++)
            if (data[i] != 0)
                return false;
        return true;
    }

    void SetNull()
    {
        memset(data, 0, sizeof(data));
    }

    inline int Compare(const base_blob& other) const { return memcmp(data, other.data, sizeof(data)); }

    friend inline bool operator==(const base_blob& a, const base_blob& b) { return a.Compare(b) == 0; }
    friend inline bool operator!=(const base_blob& a, const base_blob& b) { return a.Compare(b) != 0; }
    friend inline bool operator<(const base_blob& a, const base_blob& b) { return a.Compare(b) < 0; }

    std::string GetHex() const;
    void SetHex(const char* psz);
    void SetHex(const std::string& str) { SetHex(str.c_str()); }
    std::string ToString() const { return GetHex(); }

    unsigned char* begin()
    {
        return &data[0];
    }

    unsigned char* end()
    {
        return &data[WIDTH];
    }

    const unsigned char* begin() const
    {
        return &data[0];
    }

    const unsigned char* end() const
    {
        return &data[WIDTH];
    }

    unsigned int size() const
    {
        return sizeof(data);
    }

    uint64_t GetUint64(int pos) const
    {
        const uint8_t* ptr = data + pos * 8;
        return ((uint64_t)ptr[0]) | \
               ((uint64_t)ptr[1]) << 8 | \
               ((uint64_t)ptr[2]) << 16 | \
               ((uint64_t)ptr[3]) << 24 | \
               ((uint64_t)ptr[4]) << 32 | \
               ((uint64_t)ptr[5]) << 40 | \
               ((uint64_t)ptr[6]) << 48 | \
               ((uint64_t)ptr[7]) << 56;
    }
};

using uint160 = base_blob<160>;
using uint256 = base_blob<256>;
using uint512 = base_blob<512>;

/* uint256 from const char *.
 * This is a separate function because the constructor uint256(const char*) can result
 * in dangerously catching uint256(0).
 */
inline uint256 uint256S(const char *str)
{
    uint256 rv;
    rv.SetHex(str);
    return rv;
}
/* uint256 from std::string.
 * This is a separate function because the constructor uint256(const std::string &str) can result
 * in dangerously catching uint256(0) via std::string(const char*).
 */
inline uint256 uint256S(const std::string& str)
{
    uint256 rv;
    rv.SetHex(str);
    return rv;
}

/* uint256 from const char *.
 * This is a separate function because the constructor uint256(const char*) can result
 * in dangerously catching uint256(0).
 */
inline uint512 uint512S(const char *str)
{
    uint512 rv;
    rv.SetHex(str);
    return rv;
}
/* uint256 from std::string.
 * This is a separate function because the constructor uint256(const std::string &str) can result
 * in dangerously catching uint256(0) via std::string(const char*).
 */
inline uint512 uint512S(const std::string& str)
{
    uint512 rv;
    rv.SetHex(str);
    return rv;
}

template<>
struct std::hash<uint160> {
  size_t operator()(const uint160 &s) const {
    return s.GetUint64(0);
  }
};

template<>
struct std::hash<uint256> {
  size_t operator()(const uint256 &s) const {
    return s.GetUint64(0);
  }
};

template<>
struct std::hash<uint512> {
  size_t operator()(const uint512 &s) const {
    return s.GetUint64(0);
  }
};

template<unsigned int BitSize>
struct TbbHash {
  size_t hash(const base_blob<BitSize> &s) const {
    return s.GetUint64(0);
  }

  bool equal(const base_blob<BitSize> &s1, const base_blob<BitSize> &s2) const {
    return s1 == s2;
  }
};

#endif // BITCOIN_UINT256_H
