#pragma once

#include <string>
#include <vector>
#include <cstdlib>
#include <cstdint>

struct CMiningAddress {
  std::string MiningAddress;
  std::string PrivateKey;
  CMiningAddress() {}
  CMiningAddress(const std::string &miningAddress, const std::string &privateKey) : MiningAddress(miningAddress), PrivateKey(privateKey) {}
};

template<typename T>
class SelectorByWeight {
public:
  void add(const T &value, uint32_t weight) {
    Values.push_back(value);
    ValueIndexes.insert(ValueIndexes.end(), weight, Values.size()-1);
  }

  size_t size() const { return Values.size(); }
  const T &get() const {
    static T empty;
    return ValueIndexes.size() ? Values[ValueIndexes[rand() % ValueIndexes.size()]] : empty;
  }
  const T &getByIndex(size_t index) const { return Values[index]; }

private:
  struct Entry {
    T Value;
    uint32_t Weight;
  };

private:
  std::vector<T> Values;
  std::vector<size_t> ValueIndexes;
};
