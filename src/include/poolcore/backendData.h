#ifndef __BACKEND_DATA_H_
#define __BACKEND_DATA_H_

#include <list>
#include <string>
#include <vector>
#include "p2putils/xmstream.h"

std::string partByHeight(unsigned height);
std::string partByTime(time_t time);

struct roundElement {
  std::string userId;
  int64_t shareValue;
};

struct payoutElement {
  enum { CurrentRecordVersion = 1 };  
  
  std::string userId;
  int64_t payoutValue;
  int64_t queued;
  std::string asyncOpId;
  
  payoutElement() {}
  payoutElement(const std::string &userIdArg, int64_t paymentValueArg, int64_t queuedArg) :
    userId(userIdArg), payoutValue(paymentValueArg), queued(queuedArg) {}
    
  bool deserializeValue(const void *data, size_t size);
  bool deserializeValue(xmstream &stream);  
  void serializeValue(xmstream &stream) const;    
};  

struct shareInfo {
  std::string type;
  int64_t count;
};

struct miningRound {
  enum { CurrentRecordVersion = 1 };
  
  unsigned height;
  std::string blockHash;
  time_t time;    
    
  // aggregated share and payment value
  int64_t totalShareValue;
  int64_t availableCoins;
    
  std::list<roundElement> rounds;
  std::list<payoutElement> payouts;
    
  miningRound() {}
  miningRound(unsigned heightArg) : height(heightArg) {}
    
  friend bool operator<(const miningRound &L, const miningRound &R) { return L.height < R.height; }
    
  void clearQueued() {
    for (auto I = payouts.begin(), IE = payouts.end(); I != IE; ++I)
      I->queued = 0;
  }
    
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
  void dump();
};

struct userBalance {
  enum { CurrentRecordVersion = 1 };
  
  std::string userId;
  std::string name;
  std::string email;
  std::string passwordHash;
  int64_t balance;
  int64_t requested;
  int64_t paid;
  int64_t minimalPayout;
  userBalance() {}
  userBalance(const std::string &userIdArg, int64_t defaultMinimalPayout) :
    userId(userIdArg), balance(0), requested(0), paid(0), minimalPayout(defaultMinimalPayout) {}
      
  std::string getPartitionId() const { return "default"; }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct foundBlock {
  enum { CurrentRecordVersion = 1 };
  
  unsigned height;
  std::string hash;
  time_t time;
  int64_t availableCoins;
  std::string foundBy;
  
  std::string getPartitionId() const { return partByHeight(height); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct poolBalance {
  enum { CurrentRecordVersion = 1 };
  
  time_t time;
  int64_t balance;
  int64_t immature;
  int64_t users;
  int64_t queued;
  int64_t net;

  std::string getPartitionId() const { return partByTime(time); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;  
};

struct siteStats {
  enum { CurrentRecordVersion = 1 };
  
  std::string userId;
  time_t time;
  unsigned clients;
  unsigned workers;
  unsigned cpus;
  unsigned gpus;
  unsigned asics;
  unsigned other;
  unsigned latency;
  uint64_t power;
  
  unsigned lcount;
  
  siteStats(const std::string &userIdArg, time_t timeArg) :
    userId(userIdArg), time(timeArg),
    clients(0), workers(0), cpus(0), gpus(0), asics(0), other(0),
    latency(0), power(0),
    lcount(0) {}
  
  std::string getPartitionId() const { return partByTime(time); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;    
};

struct clientStats {
  enum { CurrentRecordVersion = 1 };
  
  std::string userId;
  std::string workerId;
  time_t time;
  uint64_t power;
  int latency;
  
  std::string address;
  int unitType;
  unsigned units;
  unsigned temp;
  
  std::string getPartitionId() const { return partByTime(time); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;    
};

struct shareStats {
  enum { CurrentRecordVersion = 1 };
  
  time_t time;
  int64_t total;
  std::vector<shareInfo> info;
  
  std::string getPartitionId() const { return partByTime(time); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;    
};

struct payoutRecord {
  enum { CurrentRecordVersion = 1 };
  
  std::string userId;
  time_t time;
  int64_t value;
  std::string transactionId;
  friend bool operator<(const payoutRecord &r1, const payoutRecord &r2) { return r1.userId < r2.userId; }
  
  std::string getPartitionId() const { return partByTime(time); }
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;      
};


#endif //__BACKEND_DATA_H_
