#ifndef __STATISTICS_H_
#define __STATISTICS_H_

#include "kvdb.h"
#include "backendData.h"
#include "poolcore/poolCore.h"
#include "poolcore/rocksdbBase.h"
#include "poolcore/shareLog.h"
#include "poolcore/usermgr.h"
#include "poolcommon/datFile.h"
#include "poolcommon/multiCall.h"
#include "poolcommon/serialize.h"
#include "poolcommon/taskHandler.h"
#include "poolcommon/timeTypes.h"
#include "asyncio/asyncio.h"
#include <tbb/concurrent_queue.h>
#include <chrono>
#include <map>
#include <set>

struct CShare;

class StatisticDb {
private:
  inline static const std::string CurrentPoolStoragePath = "stats.pool.cache.3";
  inline static const std::string CurrentWorkersStoragePath = "stats.workers.cache.3";
  inline static const std::string CurrentUsersStoragePath = "stats.users.cache.3";

public:
  enum EStatsColumn {
    EStatsColumnName = 0,
    EStatsColumnAveragePower,
    EStatsColumnSharesPerSecond,
    EStatsColumnLastShareTime
  };

  struct CredentialsWithStatistic {
    UserManager::Credentials Credentials;
    uint32_t WorkersNum = 0;
    uint64_t AveragePower = 0;
    double SharesPerSecond = 0.0;
    Timestamp LastShareTime;

    enum EColumns {
      ELogin,
      EName,
      EEmail,
      ERegistrationDate,
      EWorkersNum,
      EAveragePower,
      ESharesPerSecond,
      ELastShareTime
    };
  };

  struct CStats {
    std::string WorkerId;
    uint32_t ClientsNum = 0;
    uint32_t WorkersNum = 0;
    uint64_t AveragePower = 0;
    double SharesPerSecond = 0.0;
    UInt<256> SharesWork = UInt<256>::zero();
    Timestamp LastShareTime;
    Timestamp Time;
  };

  // +file serialization
  struct CStatsElement {
    uint64_t SharesNum = 0;
    UInt<256> SharesWork = UInt<256>::zero();
    TimeInterval Time;
    uint32_t PrimePOWTarget = -1U;
    std::vector<uint32_t> PrimePOWSharesNum;
    void reset() {
      SharesNum = 0;
      SharesWork = UInt<256>::zero();
      Time = TimeInterval();
      PrimePOWTarget = -1U;
      PrimePOWSharesNum.clear();
    }

    void merge(const CStatsElement &other) {
      SharesNum += other.SharesNum;
      SharesWork += other.SharesWork;
      PrimePOWTarget = std::min(PrimePOWTarget, other.PrimePOWTarget);
      if (other.PrimePOWSharesNum.size() > PrimePOWSharesNum.size())
        PrimePOWSharesNum.resize(other.PrimePOWSharesNum.size());
      for (size_t i = 0; i < other.PrimePOWSharesNum.size(); i++)
        PrimePOWSharesNum[i] += other.PrimePOWSharesNum[i];
    }
  };

  struct CStatsAccumulator {
    std::deque<CStatsElement> Recent;
    CStatsElement Current;
    Timestamp LastShareTime;

    void addShare(const UInt<256> &workValue, Timestamp time, unsigned primeChainLength, unsigned primePOWTarget, bool isPrimePOW) {
      Current.SharesNum++;
      Current.SharesWork += workValue;
      if (isPrimePOW) {
        Current.PrimePOWTarget = std::min(Current.PrimePOWTarget, primePOWTarget);
        primeChainLength = std::min(primeChainLength, 1024u);
        if (primeChainLength >= Current.PrimePOWSharesNum.size())
          Current.PrimePOWSharesNum.resize(primeChainLength + 1);
        Current.PrimePOWSharesNum[primeChainLength]++;
      }
      LastShareTime = time;
    }

    void merge(std::vector<CStatsElement> &cells) {
      if (cells.empty())
        return;
      int64_t ci = static_cast<int64_t>(cells.size()) - 1;
      int64_t ri = static_cast<int64_t>(Recent.size()) - 1;
      while (ci >= 0) {
        if (ri >= 0 && cells[ci].Time.TimeEnd == Recent[ri].Time.TimeEnd) {
          Recent[ri--].merge(cells[ci--]);
        } else if (ri < 0 || cells[ci].Time.TimeEnd > Recent[ri].Time.TimeEnd) {
          // Insert at the correct sorted position; in practice only appends at the end
          Recent.insert(Recent.begin() + (ri + 1), std::move(cells[ci--]));
        } else {
          ri--;
        }
      }
      if (!Recent.empty())
        LastShareTime = std::max(LastShareTime, Recent.back().Time.TimeEnd);
    }
  };

  struct CSharesWorkWithTime {
    TimeInterval Time;
    UInt<256> SharesWork;
  };

  // +file serialization
  struct CStatsExportData {
    enum { CurrentRecordVersion = 1 };

    std::string UserId;
    std::vector<CSharesWorkWithTime> Recent;

    UInt<256> recentShareValue(Timestamp acceptSharesTime) const {
      UInt<256> shareValue = UInt<256>::zero();
      for (auto &statsElement: Recent) {
        if (statsElement.Time.TimeEnd > acceptSharesTime)
          shareValue += statsElement.SharesWork;
        else
          break;
      }

      return shareValue;
    }
  };

  // +file serialization
  struct CStatsFileRecord {
    enum { CurrentRecordVersion = 1 };

    std::string Login;
    std::string WorkerId;
    CStatsElement Element;
  };

  // +file serialization
  struct CStatsFileData {
    enum { CurrentRecordVersion = 1 };

    uint64_t LastShareId = 0;
    std::vector<CStatsFileRecord> Records;
  };

private:
  using QueryPoolStatsCallback = std::function<void(const StatisticDb::CStats&)>;
  using QueryUserStatsCallback = std::function<void(const StatisticDb::CStats&, const std::vector<StatisticDb::CStats>&)>;
  using QueryAllUsersStatisticCallback = std::function<void(const std::vector<CredentialsWithStatistic>&)>;

  class TaskQueryPoolStats : public Task<StatisticDb> {
  public:
    TaskQueryPoolStats(QueryPoolStatsCallback callback) : Callback_(callback) {}
    void run(StatisticDb *statistic) final { statistic->queryPoolStatsImpl(Callback_); }
  private:
    QueryPoolStatsCallback Callback_;
  };

  class TaskQueryUserStats : public Task<StatisticDb> {
  public:
    TaskQueryUserStats(const std::string &user, QueryUserStatsCallback callback, size_t offset, size_t size, StatisticDb::EStatsColumn sortBy, bool sortDescending) :
      User_(user), Callback_(callback), Offset_(offset), Size_(size), SortBy_(sortBy), SortDescending_(sortDescending) {}
    void run(StatisticDb *statistic) final { statistic->queryUserStatsImpl(User_, Callback_, Offset_, Size_, SortBy_, SortDescending_); }
  private:
    std::string User_;
    QueryUserStatsCallback Callback_;
    size_t Offset_;
    size_t Size_;
    StatisticDb::EStatsColumn SortBy_;
    bool SortDescending_;
  };

  class TaskQueryAllUsersStats : public Task<StatisticDb> {
  public:
    TaskQueryAllUsersStats(std::vector<UserManager::Credentials> &&users, QueryAllUsersStatisticCallback callback, size_t offset, size_t size, CredentialsWithStatistic::EColumns sortBy, bool sortDescending) :
      Users_(std::move(users)), Callback_(callback), Offset_(offset), Size_(size), SortBy_(sortBy), SortDescending_(sortDescending) {}
    void run(StatisticDb *statistic) final { statistic->queryAllUserStatsImpl(Users_, Callback_, Offset_, Size_, SortBy_, SortDescending_); }
  private:
    std::vector<UserManager::Credentials> Users_;
    QueryAllUsersStatisticCallback Callback_;
    size_t Offset_;
    size_t Size_;
    CredentialsWithStatistic::EColumns SortBy_;
    bool SortDescending_;
  };

private:
  const PoolBackendConfig _cfg;
  CCoinInfo CoinInfo_;
  uint64_t LastKnownShareId_ = 0;
  // Accumulators
  // Pool stats
  CStatsAccumulator PoolStatsAcc_;
  // Worker stats (key = login + '\0' + workerId)
  std::map<std::string, CStatsAccumulator> LastWorkerStats_;
  // User stats (key = login + '\0', workerId always empty)
  std::map<std::string, CStatsAccumulator> LastUserStats_;

  static std::string makeWorkerStatsKey(const std::string &login, const std::string &workerId) {
    return login + '\0' + workerId;
  }

  static std::pair<std::string, std::string> splitWorkerStatsKey(const std::string &key) {
    size_t sep = key.find('\0');
    return {key.substr(0, sep), key.substr(sep + 1)};
  }

  CStats PoolStatsCached_;
  CFlushInfo FlushInfo_;
  Timestamp AccumulationBegin_;

  kvdb<rocksdbBase> StatsDb_;

  TaskHandlerCoroutine<StatisticDb> TaskHandler_;
  aioUserEvent *FlushEvent_;

  bool ShutdownRequested_ = false;
  bool FlushFinished_ = false;

  // Debugging only
  struct {
    uint64_t MinShareId = std::numeric_limits<uint64_t>::max();
    uint64_t MaxShareId = 0;
    uint64_t Count = 0;
  } Dbg_;

  bool parseStatsCacheFile(CDatFile &file, CStatsFileData &fileData);
  CFlushInfo loadPoolStatsFromDir(const std::filesystem::path &dirPath, CStatsAccumulator &acc);
  CFlushInfo loadMapStatsFromDir(const std::filesystem::path &dirPath, std::map<std::string, CStatsAccumulator> &accMap);

  void calcAverageMetrics(const StatisticDb::CStatsAccumulator &acc, std::chrono::seconds calculateInterval, std::chrono::seconds aggregateTime, CStats &result);
  void writeStatsToCache(const std::string &loginId, const std::string &workerId, const CStatsElement &element, xmstream &statsFileData);

  static std::vector<CStatsElement> distributeToGrid(const CStatsElement &current, int64_t beginMs, int64_t endMs, int64_t gridIntervalMs);
  void flushAccumulator(const std::string &login, const std::string &workerId, CStatsAccumulator &acc, int64_t gridIntervalMs, Timestamp timeLabel, kvdb<rocksdbBase>::Batch &batch, std::set<int64_t> &modifiedTimes, std::set<int64_t> &removedTimes);
  void rebuildDatFile(const std::string &cachePath, int64_t gridEndMs, const std::map<std::string, CStatsAccumulator> &accMap);
  void rebuildDatFile(const std::string &cachePath, int64_t gridEndMs, const std::string &login, const std::string &workerId, const CStatsAccumulator &acc);

  void flushAll(Timestamp timeLabel);
  void updatePoolStatsCached(Timestamp timeLabel);

public:
  // Initialization
  StatisticDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo);
  void replayShare(const CShare &share);
  void initializationFinish(Timestamp timeLabel);
  void start();
  void stop();
  const CCoinInfo &getCoinInfo() const { return CoinInfo_; }

  void addShare(const CShare &share);

  uint64_t lastAggregatedShareId() { return FlushInfo_.ShareId; }

  uint64_t lastKnownShareId() { return LastKnownShareId_; }

  const CStats &getPoolStats() { return PoolStatsCached_; }
  void getUserStats(const std::string &user, CStats &aggregate, std::vector<CStats> &workerStats, size_t offset, size_t size, EStatsColumn sortBy, bool sortDescending);

  /// Return recent statistic for users
  /// result - sorted by UserId
  void exportRecentStats(std::vector<CStatsExportData> &result);

  // Synchronous api
  void getHistory(const std::string &login, const std::string &workerId, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, std::vector<CStats> &history);

  // Asynchronous api
  void queryPoolStats(QueryPoolStatsCallback callback) { TaskHandler_.push(new TaskQueryPoolStats(callback)); }
  void queryUserStats(const std::string &user, QueryUserStatsCallback callback, size_t offset, size_t size, StatisticDb::EStatsColumn sortBy, bool sortDescending) {
    TaskHandler_.push(new TaskQueryUserStats(user, callback, offset, size, sortBy, sortDescending));
  }

  void queryAllusersStats(std::vector<UserManager::Credentials> &&users,
                          QueryAllUsersStatisticCallback callback,
                          size_t offset,
                          size_t size,
                          CredentialsWithStatistic::EColumns sortBy,
                          bool sortDescending) {
    TaskHandler_.push(new TaskQueryAllUsersStats(std::move(users), callback, offset, size, sortBy, sortDescending));
  }

  static void queryPoolStatsMulti(StatisticDb **backends, size_t backendsNum, std::function<void(const StatisticDb::CStats*, size_t)> callback) {
    MultiCall<StatisticDb::CStats> *context = new MultiCall<StatisticDb::CStats>(backendsNum, callback);
    for (size_t i = 0; i < backendsNum; i++)
      backends[i]->queryPoolStats(context->generateCallback(i));
  }

private:
  void queryPoolStatsImpl(QueryPoolStatsCallback callback);
  void queryUserStatsImpl(const std::string &user, QueryUserStatsCallback callback, size_t offset, size_t size, StatisticDb::EStatsColumn sortBy, bool sortDescending);

  void queryAllUserStatsImpl(const std::vector<UserManager::Credentials> &users,
                             QueryAllUsersStatisticCallback callback,
                             size_t offset,
                             size_t size,
                             CredentialsWithStatistic::EColumns sortBy,
                             bool sortDescending);
};

template<>
struct DbIo<StatisticDb::CStatsElement> {
  static inline void serialize(xmstream &out, const StatisticDb::CStatsElement &data) {
    DbIo<decltype(data.SharesNum)>::serialize(out, data.SharesNum);
    DbIo<decltype(data.SharesWork)>::serialize(out, data.SharesWork);
    DbIo<decltype(data.Time)>::serialize(out, data.Time);
    DbIo<decltype(data.PrimePOWTarget)>::serialize(out, data.PrimePOWTarget);
    DbIo<decltype(data.PrimePOWSharesNum)>::serialize(out, data.PrimePOWSharesNum);
  }

  static inline void unserialize(xmstream &in, StatisticDb::CStatsElement &data) {
    DbIo<decltype(data.SharesNum)>::unserialize(in, data.SharesNum);
    DbIo<decltype(data.SharesWork)>::unserialize(in, data.SharesWork);
    DbIo<decltype(data.Time)>::unserialize(in, data.Time);
    DbIo<decltype(data.PrimePOWTarget)>::unserialize(in, data.PrimePOWTarget);
    DbIo<decltype(data.PrimePOWSharesNum)>::unserialize(in, data.PrimePOWSharesNum);
  }
};

template<>
struct DbIo<StatisticDb::CStatsFileRecord> {
  static inline void serialize(xmstream &out, const StatisticDb::CStatsFileRecord &data) {
    DbIo<uint32_t>::serialize(out, data.CurrentRecordVersion);
    DbIo<decltype(data.Login)>::serialize(out, data.Login);
    DbIo<decltype(data.WorkerId)>::serialize(out, data.WorkerId);
    DbIo<decltype(data.Element)>::serialize(out, data.Element);
  }

  static inline void unserialize(xmstream &in, StatisticDb::CStatsFileRecord &data) {
    uint32_t version;
    DbIo<uint32_t>::unserialize(in, version);
    if (version == 1) {
      DbIo<decltype(data.Login)>::unserialize(in, data.Login);
      DbIo<decltype(data.WorkerId)>::unserialize(in, data.WorkerId);
      DbIo<decltype(data.Element)>::unserialize(in, data.Element);
    } else {
      in.seekEnd(0, true);
    }
  }
};

template<>
struct DbIo<StatisticDb::CSharesWorkWithTime> {
  static inline void serialize(xmstream &out, const StatisticDb::CSharesWorkWithTime &data) {
    DbIo<decltype(data.SharesWork)>::serialize(out, data.SharesWork);
    DbIo<decltype(data.Time)>::serialize(out, data.Time);
  }

  static inline void unserialize(xmstream &in, StatisticDb::CSharesWorkWithTime &data) {
    DbIo<decltype(data.SharesWork)>::unserialize(in, data.SharesWork);
    DbIo<decltype(data.Time)>::unserialize(in, data.Time);
  }
};

template<>
struct DbIo<StatisticDb::CStatsExportData> {
  static inline void serialize(xmstream &out, const StatisticDb::CStatsExportData &data) {
    DbIo<uint32_t>::serialize(out, data.CurrentRecordVersion);
    DbIo<decltype(data.UserId)>::serialize(out, data.UserId);
    DbIo<decltype(data.Recent)>::serialize(out, data.Recent);
  }

  static inline void unserialize(xmstream &in, StatisticDb::CStatsExportData &data) {
    uint32_t version;
    DbIo<uint32_t>::unserialize(in, version);
    if (version == 1) {
      DbIo<decltype(data.UserId)>::unserialize(in, data.UserId);
      DbIo<decltype(data.Recent)>::unserialize(in, data.Recent);
    } else {
      in.seekEnd(0, true);
    }
  }
};

template<>
struct DbIo<StatisticDb::CStatsFileData> {
  static inline void unserialize(xmstream &in, StatisticDb::CStatsFileData &data) {
    uint32_t version;
    DbIo<uint32_t>::unserialize(in, version);
    if (version == 1) {
      DbIo<decltype(data.LastShareId)>::unserialize(in, data.LastShareId);
      DbIo<decltype(data.Records)>::unserialize(in, data.Records);
    } else {
      in.seekEnd(0, true);
    }
  }
};

class StatisticShareLogConfig {
public:
  StatisticShareLogConfig() {}
  StatisticShareLogConfig(StatisticDb *statistic) : Statistic_(statistic) {}
  void initializationFinish(Timestamp time) { Statistic_->initializationFinish(time); }
  uint64_t lastAggregatedShareId() { return Statistic_->lastAggregatedShareId(); }
  uint64_t lastKnownShareId() { return Statistic_->lastKnownShareId(); }
  void replayShare(const CShare &share) { Statistic_->replayShare(share); }

private:
  StatisticDb *Statistic_;
};

class StatisticServer {
public:
  StatisticServer(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo);
  void start();
  void stop();
  StatisticDb *statisticDb() { return Statistics_.get(); }
  CCoinInfo &coinInfo() { return CoinInfo_; }

  // Asynchronous api
  void sendShare(CShare *share) { TaskHandler_.push(new TaskShare(share)); }

private:
  class TaskShare : public Task<StatisticServer> {
  public:
    TaskShare(CShare *share) : Share_(share) {}
    void run(StatisticServer *backend) final { backend->onShare(Share_.get()); }
  private:
    std::unique_ptr<CShare> Share_;
  };

private:
  void onShare(CShare *share);
  void statisticServerMain();

private:
  asyncBase *Base_;
  const PoolBackendConfig Cfg_;
  CCoinInfo CoinInfo_;
  std::unique_ptr<StatisticDb> Statistics_;
  ShareLog<StatisticShareLogConfig> ShareLog_;
  std::thread Thread_;
  TaskHandlerCoroutine<StatisticServer> TaskHandler_;
};


#endif //__STATISTICS_H_
