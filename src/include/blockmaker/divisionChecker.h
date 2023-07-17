#include <memory>
#include <vector>
#include <math.h>

class CDivisionChecker {
public:
  void init(unsigned maxPrimesNum, unsigned maxDividendSizeInLimbs) {
    if (Initialized_)
      return;

    MaxPrimesNum_ = maxPrimesNum;
    MaxDividendSizeInLimbs_ = maxDividendSizeInLimbs;

    Primes_.reset(new uint32_t[maxPrimesNum]);
    InvM_.reset(new uint64_t[maxPrimesNum]);
    Offsets_.reset(new uint32_t[maxPrimesNum]);
    Modulos_.reset(new uint32_t[maxPrimesNum*maxDividendSizeInLimbs]);

    // Generate prime table using sieve of Eratosthenes
    {
      unsigned primeTableLimit = maxPrimesNum * (log(maxPrimesNum) / log(2));
      std::vector<bool> vfComposite(primeTableLimit, false);
      for (unsigned int nFactor = 2; nFactor * nFactor < primeTableLimit; nFactor++) {
        if (vfComposite[nFactor])
          continue;
        for (unsigned int nComposite = nFactor * nFactor; nComposite < primeTableLimit; nComposite += nFactor)
          vfComposite[nComposite] = true;
      }

      unsigned primesNum = 0;
      for (unsigned int n = 2; n < primeTableLimit && primesNum < maxPrimesNum; n++) {
        if (!vfComposite[n])
          Primes_[primesNum++] = n;
      }
    }

    for (unsigned primeIdx = 0; primeIdx < maxPrimesNum; primeIdx++) {
      uint32_t prime = Primes_[primeIdx];

      findMultiplier(63, prime, &InvM_[primeIdx], &Offsets_[primeIdx]);
      if (Offsets_[primeIdx] < 64)
        findMultiplier(64, prime, &InvM_[primeIdx], &Offsets_[primeIdx]);
      Offsets_[primeIdx] -= 64;

      uint32_t modulo32 = (1ULL << 32) % prime;
      uint64_t acc = modulo32;
      for (unsigned j = 0; j < maxDividendSizeInLimbs - 1; j++) {
        Modulos_[maxDividendSizeInLimbs*primeIdx + j] = acc;
        acc = (acc * modulo32) % prime;
      }
    }
  }

  unsigned prime(unsigned n) const { return Primes_[n]; }

  uint32_t mod32(uint32_t *data, unsigned limbsNum, unsigned primeIdx) const {
    uint32_t *modulos = &Modulos_[MaxDividendSizeInLimbs_ * primeIdx];

    uint64_t acc = data[0];
    for (unsigned i = 1; i < limbsNum; i++)
      acc += (uint64_t)modulos[i-1] * (uint64_t)data[i];

    uint64_t quotient = (((unsigned __int128)acc * InvM_[primeIdx]) >> 64) >> Offsets_[primeIdx];
    return acc - quotient * Primes_[primeIdx];
  }

private:
  bool findMultiplier(unsigned maxDividendBits,
                      unsigned divisor,
                      uint64_t *multiplier,
                      unsigned *offset) {
    if (divisor == 2) {
      *multiplier = 0x8000000000000000ULL;
      *offset = 64;
      return true;
    }

    unsigned __int128 maxDividend = 1;
    maxDividend <<= maxDividendBits;
    maxDividend--;

    unsigned __int128 nc = (maxDividend / divisor) * divisor - 1;
    for (unsigned i = 0; i < 2*maxDividendBits + 1; i++) {
      unsigned __int128 powOf2 = 1;
      powOf2 <<= i;
      if (powOf2 > nc*(divisor - 1 - ((powOf2 - 1) % divisor))) {
        *multiplier = (powOf2 + divisor - 1 - ((powOf2 - 1) % divisor)) / divisor;
        *offset = i;
        return true;
      }
    }

    return false;
  }

private:
  bool Initialized_ = false;
  unsigned MaxPrimesNum_ = 0;
  unsigned MaxDividendSizeInLimbs_ = 0;
  std::unique_ptr<uint32_t[]> Primes_;
  std::unique_ptr<uint64_t[]> InvM_;
  std::unique_ptr<uint32_t[]> Offsets_;
  std::unique_ptr<uint32_t[]> Modulos_;
};
