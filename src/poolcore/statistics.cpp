#include "poolcore/statistics.h"
#include "loguru.hpp"
#include <algorithm>

StatisticDb::StatisticDb(const PoolBackendConfig &config, const CCoinInfo &coinInfo) : _cfg(config), CoinInfo_(coinInfo),
  _poolStats("pool", 0),
  _workerStatsDb(_cfg.dbPath / coinInfo.Name / "workerStats"),
  _poolStatsDb(_cfg.dbPath / coinInfo.Name / "poolstats")
{
}

void StatisticDb::addStats(const Stats *stats)
{
  std::string key = stats->userId;
    key.push_back('/');
    key.append(stats->workerId);
  
  ClientStatsRecord s;
  s.Login = stats->userId;
  s.WorkerId = stats->workerId;
  s.Time = time(0);
  s.Power = stats->power;
  s.Latency = stats->latency;
  s.Address = stats->address;
  s.UnitType = stats->type;
  s.Units = stats->units;
  s.Temp = stats->temp;
  _statsMap[key] = s;
}

void StatisticDb::update()
{
  uint64_t avgLatency = 0;
  uint64_t power = 0;
  unsigned lcount = 0;
  unsigned count = 0;
  unsigned units[EOTHER+1];
  time_t currentTime = time(0);
  time_t removeTimeLabel = currentTime - _cfg.KeepStatsTime;
  memset(units, 0, sizeof(units));
  
  std::map<std::string, SiteStatsRecord> uniqueClients;
  for (auto I = _statsMap.begin(), IE = _statsMap.end(); I != IE;) {
    const ClientStatsRecord &stats = I->second;
    if (stats.Time >= removeTimeLabel) {
      if (stats.Power < 16000)
        power += stats.Power;
      if (stats.Latency >= 0) {
        avgLatency += stats.Latency;
        lcount++;
      }

      {
        auto clIt = uniqueClients.find(stats.Login);
        if (clIt == uniqueClients.end())
          clIt = uniqueClients.insert(clIt, std::make_pair(stats.Login, SiteStatsRecord("cl:" + stats.Login, currentTime)));
        SiteStatsRecord &clientAggregate = clIt->second;
        clientAggregate.Clients = 1;
        clientAggregate.Workers++;
        switch (stats.UnitType) {
          case ECPU :
            clientAggregate.CPUNum += stats.Units;
            break;
          case EGPU :
            clientAggregate.GPUNum += stats.Units;
            break;
          case EASIC :
            clientAggregate.ASICNum += stats.Units;
            break;
          default :
            clientAggregate.OtherNum += stats.Units;
            break;
        }
        
        if (stats.Latency >= 0) {
          clientAggregate.Latency += stats.Latency;
          clientAggregate.LCount++;
        }
        
        clientAggregate.Power += stats.Power;
      }
      

      units[std::min(stats.UnitType, static_cast<uint32_t>(EOTHER))] += stats.Units;
      ++count;
      ++I;
      
      // save worker statistics
      _workerStatsDb.put(stats);
    } else {
      _statsMap.erase(I++);
    }
  }
  
  // save client statistics
  for (auto &cs: uniqueClients)
    _poolStatsDb.put(cs.second);
  
  {
    _poolStats.Time = currentTime;
    _poolStats.Clients = uniqueClients.size();
    _poolStats.Workers = count;
    _poolStats.CPUNum = units[ECPU];
    _poolStats.GPUNum = units[EGPU];
    _poolStats.ASICNum = units[EASIC];
    _poolStats.OtherNum = units[EOTHER];
    _poolStats.Latency = lcount ? (double)avgLatency / lcount : -1;
    _poolStats.Power = power;
    _poolStatsDb.put(_poolStats);
    LOG_F(INFO,
          "clients: %u, workers: %u, cpus: %u, gpus: %u, asics: %u, other: %u, latency: %u, power: %u",
          (unsigned)_poolStats.Clients,
          (unsigned)_poolStats.Workers,
          (unsigned)_poolStats.CPUNum,
          (unsigned)_poolStats.GPUNum,
          (unsigned)_poolStats.ASICNum,
          (unsigned)_poolStats.OtherNum,
          (unsigned)_poolStats.Latency,
          (unsigned)_poolStats.Power);
             
  }
}

uint64_t StatisticDb::getClientPower(const std::string &userId) const
{
  uint64_t power = 0;
  for (auto It = _statsMap.lower_bound(userId); It != _statsMap.end(); ++It) {
    const ClientStatsRecord &stats = It->second;
    if (stats.Login != userId)
      break;
    power += stats.Power;
  }
  return power;
}

uint64_t StatisticDb::getPoolPower() const
{
  return _poolStats.Power;
}
