#include "gtest/gtest.h"
#include "poolcore/statsData.h"
#include <set>
#include <chrono>

using namespace std::chrono;

static const Timestamp T1min(60000);
static const Timestamp T2min(120000);
static const Timestamp T3min(180000);

TEST(StatsData, Flush)
{
  std::set<Timestamp> modified, removed;

  // Single cell: Current → Recent, modifiedTimes populated
  {
    CStatsSeries series(minutes(1), minutes(10));
    CWorkSummary work;
    work.SharesNum = 100;
    work.SharesWork = UInt<256>(1000u);
    series.addWorkSummary(work, T1min);
    ASSERT_EQ(series.Current.SharesNum, 100u);

    series.flush(Timestamp(0), T1min, "user1", "w1", T1min, nullptr, modified, removed);

    EXPECT_EQ(series.Current.SharesNum, 0u);
    EXPECT_TRUE(series.Current.SharesWork.isZero());
    ASSERT_EQ(series.Recent.size(), 1u);
    EXPECT_EQ(series.Recent[0].Data.SharesNum, 100u);
    EXPECT_EQ(series.Recent[0].Time.TimeBegin, Timestamp(0));
    EXPECT_EQ(series.Recent[0].Time.TimeEnd, T1min);
    EXPECT_EQ(modified.size(), 1u);
    EXPECT_TRUE(modified.count(T1min));
    EXPECT_TRUE(removed.empty());
  }

  // Multi-cell: 2.5 minutes → 3 grid cells, shares conserved
  {
    CStatsSeries series(minutes(1), minutes(10));
    CWorkSummary work;
    work.SharesNum = 1000;
    work.SharesWork = UInt<256>(5000u);
    Timestamp t150s(150000);
    series.addWorkSummary(work, t150s);

    modified.clear(); removed.clear();
    series.flush(Timestamp(0), t150s, "user1", "w1", t150s, nullptr, modified, removed);

    ASSERT_EQ(series.Recent.size(), 3u);
    EXPECT_EQ(series.Recent[0].Time.TimeEnd, T1min);
    EXPECT_EQ(series.Recent[1].Time.TimeEnd, T2min);
    EXPECT_EQ(series.Recent[2].Time.TimeEnd, T3min);
    uint64_t totalShares = 0;
    for (auto &cell : series.Recent)
      totalShares += cell.Data.SharesNum;
    EXPECT_EQ(totalShares, 1000u);
    EXPECT_EQ(modified.size(), 3u);
  }

  // Point-in-time (begin == end) → single cell
  {
    CStatsSeries series(minutes(1), minutes(10));
    CWorkSummary work;
    work.SharesNum = 500;
    work.SharesWork = UInt<256>(2000u);
    Timestamp t30s(30000);
    series.addWorkSummary(work, t30s);

    modified.clear(); removed.clear();
    series.flush(t30s, t30s, "user1", "w1", t30s, nullptr, modified, removed);

    ASSERT_EQ(series.Recent.size(), 1u);
    EXPECT_EQ(series.Recent[0].Data.SharesNum, 500u);
    EXPECT_EQ(series.Recent[0].Time.TimeBegin, Timestamp(0));
    EXPECT_EQ(series.Recent[0].Time.TimeEnd, T1min);
  }

  // Merge into existing cell + misaligned entry stays untouched
  {
    CStatsSeries series(minutes(1), minutes(10));
    CWorkSummaryWithTime existing;
    existing.Time = TimeInterval(Timestamp(0), T1min);
    existing.Data.SharesNum = 200;
    existing.Data.SharesWork = UInt<256>(500u);
    series.Recent.push_back(existing);

    CWorkSummary work;
    work.SharesNum = 100;
    work.SharesWork = UInt<256>(300u);
    series.addWorkSummary(work, Timestamp(30000));

    modified.clear(); removed.clear();
    series.flush(Timestamp(0), T1min, "user1", "w1", T1min, nullptr, modified, removed);
    ASSERT_EQ(series.Recent.size(), 1u);
    EXPECT_EQ(series.Recent[0].Data.SharesNum, 300u);

    // Inject misaligned entry [5s, 55s), flush more work → merges into aligned cell only
    CWorkSummaryWithTime misaligned;
    misaligned.Time = TimeInterval(Timestamp(5000), Timestamp(55000));
    misaligned.Data.SharesNum = 77;
    misaligned.Data.SharesWork = UInt<256>(777u);
    series.Recent.push_front(misaligned);

    CWorkSummary work2;
    work2.SharesNum = 50;
    work2.SharesWork = UInt<256>(200u);
    series.addWorkSummary(work2, Timestamp(45000));

    modified.clear(); removed.clear();
    series.flush(Timestamp(0), T1min, "user1", "w1", T1min, nullptr, modified, removed);
    ASSERT_EQ(series.Recent.size(), 2u);
    EXPECT_EQ(series.Recent[0].Data.SharesNum, 77u);
    EXPECT_EQ(series.Recent[0].Time.TimeEnd, Timestamp(55000));
    EXPECT_EQ(series.Recent[1].Data.SharesNum, 350u);
    EXPECT_EQ(series.Recent[1].Time.TimeEnd, T1min);
  }

  // Removes old records (keepTime=0, removeTimePoint = currentTime)
  {
    CStatsSeries series(minutes(1), minutes(0));
    for (int i = 0; i < 2; i++) {
      CWorkSummaryWithTime entry;
      entry.Time = TimeInterval(Timestamp(i * 60000), Timestamp((i + 1) * 60000));
      entry.Data.SharesNum = 50;
      entry.Data.SharesWork = UInt<256>(100u);
      series.Recent.push_back(entry);
    }

    // currentTime=90s → removes [0,60s), keeps [60s,120s)
    modified.clear(); removed.clear();
    series.flush(Timestamp(0), Timestamp(0), "user1", "w1", Timestamp(90000), nullptr, modified, removed);
    ASSERT_EQ(series.Recent.size(), 1u);
    EXPECT_EQ(series.Recent[0].Time.TimeEnd, T2min);
    EXPECT_TRUE(modified.empty());
    EXPECT_EQ(removed.size(), 1u);
  }

  // Empty Current + some Recent → only removal happens
  {
    CStatsSeries series(minutes(1), minutes(0));
    CWorkSummaryWithTime entry;
    entry.Time = TimeInterval(Timestamp(0), T1min);
    entry.Data.SharesNum = 10;
    series.Recent.push_back(entry);

    modified.clear(); removed.clear();
    series.flush(Timestamp(0), Timestamp(0), "", "", Timestamp(60001), nullptr, modified, removed);
    EXPECT_TRUE(series.Recent.empty());
    EXPECT_EQ(removed.size(), 1u);
  }

  // Old cell skips Recent but writes to DB batch
  {
    CStatsSeries series(minutes(1), minutes(0));
    CWorkSummary work;
    work.SharesNum = 100;
    work.SharesWork = UInt<256>(500u);
    series.addWorkSummary(work, Timestamp(30000));

    kvdb<rocksdbBase>::Batch batch;
    modified.clear(); removed.clear();
    series.flush(Timestamp(0), Timestamp(30000), "user1", "w1", Timestamp(300000), &batch, modified, removed);
    EXPECT_EQ(series.Current.SharesNum, 0u);
    EXPECT_TRUE(series.Recent.empty());
    EXPECT_TRUE(modified.empty());
    EXPECT_FALSE(batch.empty());
  }

  // Mixed old/new: [0,120s) with keepTime=0, currentTime=90s
  // Cell [0,60s) too old for Recent, cell [60s,120s) → Recent with half of shares
  {
    CStatsSeries series(minutes(1), minutes(0));
    CWorkSummary work;
    work.SharesNum = 1000;
    work.SharesWork = UInt<256>(2000u);
    series.addWorkSummary(work, T2min);

    modified.clear(); removed.clear();
    series.flush(Timestamp(0), T2min, "user1", "w1", Timestamp(90000), nullptr, modified, removed);
    ASSERT_EQ(series.Recent.size(), 1u);
    EXPECT_EQ(series.Recent[0].Time.TimeEnd, T2min);
    EXPECT_EQ(series.Recent[0].Data.SharesNum, 500u);
    EXPECT_EQ(modified.size(), 1u);
  }
}

TEST(StatsData, CalcAverageMetrics)
{
  CCoinInfo coinInfo;
  coinInfo.PowerUnitType = CCoinInfo::EHash;
  coinInfo.PowerMultLog10 = 0; // work/sec without scaling

  CStatsSeries series(minutes(1), minutes(10));
  CStats result;

  // Empty series
  series.calcAverageMetrics(coinInfo, seconds(60), T1min, result);
  EXPECT_DOUBLE_EQ(result.SharesPerSecond, 0.0);
  EXPECT_TRUE(result.SharesWork.isZero());
  EXPECT_EQ(result.AveragePower, 0u);

  // Only Current
  series.Current.SharesNum = 100;
  series.Current.SharesWork = UInt<256>(6000u);
  series.calcAverageMetrics(coinInfo, seconds(60), T1min, result);
  EXPECT_DOUBLE_EQ(result.SharesPerSecond, 100.0 / 60);
  EXPECT_EQ(result.SharesWork, UInt<256>(6000u));
  EXPECT_EQ(result.AveragePower, 100u); // 6000/60

  // Current + Recent fully in window
  CWorkSummaryWithTime cell;
  cell.Time = TimeInterval(Timestamp(0), T1min);
  cell.Data.SharesNum = 200;
  cell.Data.SharesWork = UInt<256>(12000u);
  series.Recent.push_back(cell);
  // window = [120s-300s .. 120s] = [-180s .. 120s] → all Recent fits
  series.calcAverageMetrics(coinInfo, seconds(300), T2min, result);
  EXPECT_DOUBLE_EQ(result.SharesPerSecond, 300.0 / 300);
  EXPECT_EQ(result.SharesWork, UInt<256>(18000u));
  EXPECT_EQ(result.AveragePower, 60u); // 18000/300

  // Old cell excluded (windowBegin = 120s-60s = 60s, cell TimeEnd=60s <= 60s → out)
  series.calcAverageMetrics(coinInfo, seconds(60), T2min, result);
  EXPECT_DOUBLE_EQ(result.SharesPerSecond, 100.0 / 60);
  EXPECT_EQ(result.SharesWork, UInt<256>(6000u));

  // Boundary clipping (windowBegin = 120s-90s = 30s, cell [0,60s) straddles)
  // fraction = (60s-30s)/(60s-0s) = 0.5 → 100 shares, 6000 work from cell
  // + Current 100 shares, 6000 work → total 200 shares, 12000 work
  series.calcAverageMetrics(coinInfo, seconds(90), T2min, result);
  EXPECT_DOUBLE_EQ(result.SharesPerSecond, 200.0 / 90);
  EXPECT_EQ(result.AveragePower, 133u); // 12000/90 = 133

  // LastShareTime propagated
  series.LastShareTime = Timestamp(90000);
  series.calcAverageMetrics(coinInfo, seconds(60), T2min, result);
  EXPECT_EQ(result.LastShareTime, Timestamp(90000));
}
