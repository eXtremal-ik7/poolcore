#include "poolcore/statistics.h"
#include "loguru.hpp"
#include "p2p/p2p.h"
#include <algorithm>

using namespace boost::filesystem;

StatisticDb::StatisticDb(StatisticDb::config *cfg, p2pNode *client) : _cfg(*cfg),
  _poolStats("pool", 0),
  _workerStatsDb(path(_cfg.dbPath / "workerStats").c_str()),
  _poolStatsDb(path(_cfg.dbPath / "poolstats").c_str())
{
}


void StatisticDb::addStats(const Stats *stats)
{
  std::string key = stats->userId()->c_str();
    key.push_back('/');
    key.append(stats->workerId()->c_str());
  
  clientStats s;
  s.userId = stats->userId()->c_str();
  s.workerId = stats->workerId()->c_str();
  s.time = time(0);
  s.power = stats->power();
  s.latency = stats->latency();
  s.address = stats->address()->c_str();
  s.unitType = stats->type();
  s.units = stats->units();
  s.temp = stats->temp();
  _statsMap[key] = s;
}

void StatisticDb::update()
{
  uint64_t avgLatency = 0;
  uint64_t power = 0;
  unsigned lcount = 0;
  unsigned count = 0;
  unsigned units[UnitType_OTHER+1];
  time_t currentTime = time(0);
  time_t removeTimeLabel = currentTime - _cfg.keepStatsTime;
  memset(units, 0, sizeof(units));
  
  std::map<std::string, siteStats> uniqueClients;
  for (auto I = _statsMap.begin(), IE = _statsMap.end(); I != IE;) {
    const clientStats &stats = I->second;
    if (stats.time >= removeTimeLabel) {
      if (stats.power < 1000000)
        power += stats.power;
      if (stats.latency >= 0) {
        avgLatency += stats.latency;
        lcount++;
      }

      {
        auto clIt = uniqueClients.find(stats.userId);
        if (clIt == uniqueClients.end())
          clIt = uniqueClients.insert(clIt, std::make_pair(stats.userId, siteStats("cl:" + stats.userId, currentTime)));
        siteStats &clientAggregate = clIt->second;
        clientAggregate.clients = 1;
        clientAggregate.workers++;
        switch (stats.unitType) {
          case UnitType_CPU :
            clientAggregate.cpus += stats.units;
            break;
          case UnitType_GPU :
            clientAggregate.gpus += stats.units;
            break;
          case UnitType_ASIC :
            clientAggregate.asics += stats.units;
            break;
          default :
            clientAggregate.other += stats.units;
            break;
        }
        
        if (stats.latency >= 0) {
          clientAggregate.latency += stats.latency;
          clientAggregate.lcount++;
        }
        
        clientAggregate.power += stats.power;
      }
      

      units[std::min(stats.unitType, (int)UnitType_OTHER)] += stats.units;
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
    _poolStats.time = currentTime;
    _poolStats.clients = uniqueClients.size();
    _poolStats.workers = count;
    _poolStats.cpus = units[UnitType_CPU];
    _poolStats.gpus = units[UnitType_GPU];
    _poolStats.asics = units[UnitType_ASIC];
    _poolStats.other = units[UnitType_OTHER];
    _poolStats.latency = lcount ? (double)avgLatency / lcount : -1;
    _poolStats.power = power;
    _poolStatsDb.put(_poolStats);
    LOG_F(INFO,
          "clients: %u, workers: %u, cpus: %u, gpus: %u, asics: %u, other: %u, latency: %u, power: %u",
          (unsigned)_poolStats.clients,
          (unsigned)_poolStats.workers,
          (unsigned)_poolStats.cpus,
          (unsigned)_poolStats.gpus,
          (unsigned)_poolStats.asics,
          (unsigned)_poolStats.other,
          (unsigned)_poolStats.latency,
          (unsigned)_poolStats.power);
             
  }
}

uint64_t StatisticDb::getClientPower(const std::string &userId) const
{
  uint64_t power = 0;
  for (auto It = _statsMap.lower_bound(userId); It != _statsMap.end(); ++It) {
    const clientStats &stats = It->second;
    if (stats.userId != userId)
      break;
    power += stats.power;
  }
  return power;
}

uint64_t StatisticDb::getPoolPower() const
{
  return _poolStats.power;
}

void StatisticDb::queryClientStats(p2pPeer *peer, uint32_t id, const std::string &userId)
{
  flatbuffers::FlatBufferBuilder fbb;  
  uint64_t avgLatency = 0;
  uint64_t power = 0;
  unsigned lcount = 0;
  unsigned units[UnitType_OTHER+1];
  memset(units, 0, sizeof(units));  
  
  std::vector<flatbuffers::Offset<WorkerStatsRecord>> workers;
  for (auto It = _statsMap.lower_bound(userId); It != _statsMap.end(); ++It) {
    const clientStats &stats = It->second;
    if (stats.userId != userId)
      break;
    
    power += stats.power;
    if (stats.latency >= 0) {
      avgLatency += stats.latency;
      lcount++;
    }
    
    units[std::min(stats.unitType, (int)UnitType_OTHER)] += stats.units;
    
    workers.push_back(CreateWorkerStatsRecord(fbb,
                                              fbb.CreateString(stats.workerId),
                                              0,
                                              fbb.CreateString(stats.address),
                                              stats.power,
                                              stats.latency,
                                              (UnitType)stats.unitType,
                                              stats.units,
                                              stats.temp));

  }
  
  auto aggregate = CreateWorkerStatsAggregate(fbb, 0, 0,
                                              workers.size(),
                                              units[UnitType_CPU],
                                              units[UnitType_GPU],
                                              units[UnitType_ASIC],
                                              units[UnitType_OTHER],
                                              lcount ? (double)avgLatency / lcount : -1,
                                              power);

  auto workersOffset = fbb.CreateVector(workers);
  
  QueryResultBuilder qrb(fbb);  
  qrb.add_workers(workersOffset);
  qrb.add_aggregate(aggregate);
  fbb.Finish(qrb.Finish());
  aiop2pSend(peer->connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}

void StatisticDb::queryPoolStats( p2pPeer *peer, uint32_t id)
{
  flatbuffers::FlatBufferBuilder fbb;  
  
  auto offset = CreateWorkerStatsAggregate(fbb, 0,
    _poolStats.clients,
    _poolStats.workers,
    _poolStats.cpus,
    _poolStats.gpus,
    _poolStats.asics,
    _poolStats.other,
    _poolStats.latency,
    _poolStats.power);
  
  QueryResultBuilder qrb(fbb);    
  qrb.add_aggregate(offset);
  fbb.Finish(qrb.Finish());
  aiop2pSend(peer->connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}
