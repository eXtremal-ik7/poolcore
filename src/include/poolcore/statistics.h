#ifndef __STATISTICS_H_
#define __STATISTICS_H_

#include "poolcommon/pool_generated.h"
#include "kvdb.h"
#include "backendData.h"
#include "poolcore/leveldbBase.h"
#include <map>

class p2pNode;
class p2pPeer;

class StatisticDb {
public:
  struct config {
    std::string dbPath;
    unsigned keepStatsTime;
  };
  
private:
  config _cfg;
  
  std::map<std::string, clientStats> _statsMap;
  siteStats _poolStats;
  
  kvdb<levelDbBase> _workerStatsDb;
  kvdb<levelDbBase> _poolStatsDb;
  
public:
  StatisticDb(config *cfg, p2pNode *client);
  
  void addStats(const Stats *stats);
  void update();
  
  uint64_t getClientPower(const std::string &userId) const;
  uint64_t getPoolPower() const;
  void queryClientStats(p2pPeer *peer, uint32_t id, const std::string &userId);
  void queryPoolStats(p2pPeer *peer, uint32_t id);
};


#endif //__STATISTICS_H_
