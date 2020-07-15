#include "poolcore/statistics.h"
#include "loguru.hpp"
#include "p2p/p2p.h"
#include <algorithm>

StatisticDb::StatisticDb(const PoolBackendConfig &config, const CCoinInfo &coinInfo) : _cfg(config), CoinInfo_(coinInfo),
  _poolStats("pool", 0),
  _workerStatsDb(_cfg.dbPath / coinInfo.Name / "workerStats"),
  _poolStatsDb(_cfg.dbPath / coinInfo.Name / "poolstats")
{
}

void StatisticDb::addStats(const Stats *stats)
{
  std::string key = stats->userId()->c_str();
    key.push_back('/');
    key.append(stats->workerId()->c_str());
  
  ClientStatsRecord s;
  s.Login = stats->userId()->c_str();
  s.WorkerId = stats->workerId()->c_str();
  s.Time = time(0);
  s.Power = stats->power();
  s.Latency = stats->latency();
  s.Address = stats->address()->c_str();
  s.UnitType = stats->type();
  s.Units = stats->units();
  s.Temp = stats->temp();
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
          case UnitType_CPU :
            clientAggregate.CPUNum += stats.Units;
            break;
          case UnitType_GPU :
            clientAggregate.GPUNum += stats.Units;
            break;
          case UnitType_ASIC :
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
      

      units[std::min(stats.UnitType, static_cast<uint32_t>(UnitType_OTHER))] += stats.Units;
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
    _poolStats.CPUNum = units[UnitType_CPU];
    _poolStats.GPUNum = units[UnitType_GPU];
    _poolStats.ASICNum = units[UnitType_ASIC];
    _poolStats.OtherNum = units[UnitType_OTHER];
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
    const ClientStatsRecord &stats = It->second;
    if (stats.Login != userId)
      break;
    
    power += stats.Power;
    if (stats.Latency >= 0) {
      avgLatency += stats.Latency;
      lcount++;
    }
    
    units[std::min(stats.UnitType, static_cast<uint32_t>(UnitType_OTHER))] += stats.Units;
    
    workers.push_back(CreateWorkerStatsRecord(fbb,
                                              fbb.CreateString(stats.WorkerId),
                                              0,
                                              fbb.CreateString(stats.Address),
                                              stats.Power,
                                              stats.Latency,
                                              (UnitType)stats.UnitType,
                                              stats.Units,
                                              stats.Temp));

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
    _poolStats.Clients,
    _poolStats.Workers,
    _poolStats.CPUNum,
    _poolStats.GPUNum,
    _poolStats.ASICNum,
    _poolStats.OtherNum,
    _poolStats.Latency,
    _poolStats.Power);
  
  QueryResultBuilder qrb(fbb);    
  qrb.add_aggregate(offset);
  fbb.Finish(qrb.Finish());
  aiop2pSend(peer->connection, fbb.GetBufferPointer(), id, p2pMsgResponse, fbb.GetSize(), afNone, 3000000, nullptr, nullptr);
}
