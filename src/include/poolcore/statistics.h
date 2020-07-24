#ifndef __STATISTICS_H_
#define __STATISTICS_H_

#include "kvdb.h"
#include "backendData.h"
#include "poolcore/poolCore.h"
#include "poolcore/rocksdbBase.h"
#include <map>

class p2pNode;
class p2pPeer;

enum EUnitType {
  EUnitTypeCPU = 0,
  EUnitTypeGPU,
  EUnitTypeASIC,
  EUnitTypeOther
};

struct CUserStats {
  std::string userId;
  std::string workerId;
  uint64_t power;
  int32_t latency;
  std::string address;
  EUnitType type;
  uint32_t units;
  uint32_t temp;
};

class StatisticDb {
private:
  const PoolBackendConfig &_cfg;
  CCoinInfo CoinInfo_;
  
  std::map<std::string, ClientStatsRecord> _statsMap;
  SiteStatsRecord _poolStats;
  
  kvdb<rocksdbBase> _workerStatsDb;
  kvdb<rocksdbBase> _poolStatsDb;
  
public:
  StatisticDb(const PoolBackendConfig &config, const CCoinInfo &coinInfo);
  
  void addStats(const CUserStats *stats);
  void update();
  
  uint64_t getClientPower(const std::string &userId) const;
  uint64_t getPoolPower() const;
  const SiteStatsRecord &getPoolStats() { return _poolStats; }
  void getUserStats(const std::string &user, SiteStatsRecord &aggregate, std::vector<ClientStatsRecord> &workers);
};


#endif //__STATISTICS_H_
