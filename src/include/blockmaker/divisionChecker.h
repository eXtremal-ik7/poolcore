#include <gmpxx.h>
#include <memory>
#include <vector>
#include "loguru.hpp"

class CDivisionChecker {
public:
  unsigned lastDivisor(const mpz_t n, unsigned firstPrimeIndex, unsigned lastPrimeIndex) const {
    char s[4096];
    uint32_t limbs[maxDividendSize];
    size_t limbsNumber;
    mpz_export(limbs, &limbsNumber, -1, 4, 0, 0, n);

    lastPrimeIndex = lastPrimeIndex == -1U ? maxPrimesNum : lastPrimeIndex;
    for (unsigned i = firstPrimeIndex; i < lastPrimeIndex; i++) {
      if (mod32(limbs, limbsNumber, &modulos[(maxDividendSize-1)*i], vPrimes[i], inversedMultipliers[i], offsets[i]) == 0) {
        return i;
      }
    }

    return -1U;
  }

  unsigned prime(unsigned n) { return vPrimes[n]; }

public:
  void init() {
    if (Initialized_)
      return;

    vPrimes.reset(new uint32_t[maxPrimesNum]);
    modulos.reset(new uint32_t[maxPrimesNum*maxDividendSize]);
    inversedMultipliers.reset(new uint64_t[maxPrimesNum]);
    offsets.reset(new uint32_t[maxPrimesNum]);

    // Generate prime table using sieve of Eratosthenes
    std::vector<bool> vfComposite (nPrimeTableLimit, false);
    for (unsigned int nFactor = 2; nFactor * nFactor < nPrimeTableLimit; nFactor++)
    {
        if (vfComposite[nFactor])
            continue;
        for (unsigned int nComposite = nFactor * nFactor; nComposite < nPrimeTableLimit; nComposite += nFactor)
            vfComposite[nComposite] = true;
    }
    unsigned primesNum = 0;
    for (unsigned int n = 2; n < nPrimeTableLimit; n++) {
        if (!vfComposite[n])
          vPrimes[primesNum++] = n;
    }
    LOG_F(INFO, "CDivisionChecker() : prime table [1, %u(%u)] generated with %u primes", vPrimes[primesNum-1], nPrimeTableLimit, primesNum);

    for (unsigned i = 0; i < primesNum; i++) {
      unsigned prime = vPrimes[i];

      mpz_class inversedMultiplier;
      findMultiplierForConstantDivision(63, prime, &inversedMultiplier, &offsets[i]);
      if (offsets[i] < 64)
        findMultiplierForConstantDivision(64, prime, &inversedMultiplier, &offsets[i]);

      size_t limbsNumber;
      mpz_export(&inversedMultipliers[i], &limbsNumber, -1, 4, 0, 0, inversedMultiplier.get_mpz_t());

      mpz_class X = 1;
      for (unsigned j = 0; j < maxDividendSize-1; j++) {
        X <<= 32;
        mpz_class mod = X % prime;
        modulos[(maxDividendSize-1)*i + j] = mod.get_ui();
      }
    }

    Initialized_ = true;
  }

private:
  bool findMultiplierForConstantDivision(unsigned maxDividendBits,
                                         const mpz_class &divisor,
                                         mpz_class *multiplier,
                                         unsigned *offset)
  {
    mpz_class two = 2;

    mpz_class maxDividend = 1;
    maxDividend <<= maxDividendBits;
    maxDividend--;

    mpz_class nc = (maxDividend / divisor) * divisor - 1;
    for (unsigned i = 0; i < 2*maxDividendBits + 1; i++) {
      mpz_class powOf2 = 1;
      powOf2 <<= i;
      if (powOf2 > nc*(divisor - 1 - ((powOf2 - 1) % divisor))) {
        *multiplier = (powOf2 + divisor - 1 - ((powOf2 - 1) % divisor)) / divisor;
        *offset = i;
        return true;
      }
    }

    return false;
  }

  uint32_t mod32(uint32_t *data, unsigned size, uint32_t *modulos, uint32_t divisor, uint64_t inversedMultiplier, unsigned offset) const
  {
    uint64_t acc = data[0];
    for (unsigned i = 1; i < size; i++)
      acc += (uint64_t)modulos[i-1] * (uint64_t)data[i];

    unsigned __int128 quotient = ((unsigned __int128)acc*inversedMultiplier) >> offset;
    return acc - quotient*divisor;
  }

private:
  bool Initialized_ = false;
  static constexpr int nPrimeTableLimit = 1742538;
  static constexpr unsigned maxDividendSize = 384/32;
  static constexpr unsigned maxPrimesNum = 131072;
  std::unique_ptr<uint32_t[]> vPrimes;
  std::unique_ptr<uint32_t[]> modulos;
  std::unique_ptr<uint64_t[]> inversedMultipliers;
  std::unique_ptr<uint32_t[]> offsets;
};
