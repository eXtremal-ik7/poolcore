#ifndef __STATISTICS_H_
#define __STATISTICS_H_

#include "poolcommon/pool_generated.h"
#include "kvdb.h"
#include "backendData.h"
#include "poolcore/poolCore.h"
#include "poolcore/rocksdbBase.h"
#include <map>

class p2pNode;
class p2pPeer;

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
  
  void addStats(const Stats *stats);
  void update();
  
  uint64_t getClientPower(const std::string &userId) const;
  uint64_t getPoolPower() const;
  void queryClientStats(p2pPeer *peer, uint32_t id, const std::string &userId);
  void queryPoolStats(p2pPeer *peer, uint32_t id);
};


#endif //__STATISTICS_H_
