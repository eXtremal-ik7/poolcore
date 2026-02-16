#include "gtest/gtest.h"
#include "poolcore/statsData.h"
#include <set>
#include <chrono>

using namespace std::chrono;

static const Timestamp T1min(60000);
static const Timestamp T2min(120000);
static const Timestamp T3min(180000);

TEST(StatsData, FlushMovesCurrentToRecent)
{
  CStatsSeries series(minutes(1), minutes(10));

  // Add work: 100 shares over interval [0, 60s), grid = 1min
  CWorkSummary work;
  work.SharesNum = 100;
  work.SharesWork = UInt<256>(1000u);
  series.addWorkSummary(work, T1min);

  ASSERT_EQ(series.Current.SharesNum, 100u);
  ASSERT_TRUE(series.Recent.empty());

  std::set<Timestamp> modified, removed;
  series.flush(Timestamp(0), T1min, "user1", "w1", T1min, nullptr, modified, removed);

  // Current must be reset
  EXPECT_EQ(series.Current.SharesNum, 0u);
  EXPECT_TRUE(series.Current.SharesWork.isZero());

  // Recent must have data
  ASSERT_EQ(series.Recent.size(), 1u);
  EXPECT_EQ(series.Recent[0].Data.SharesNum, 100u);
  EXPECT_EQ(series.Recent[0].Time.TimeBegin, Timestamp(0));
  EXPECT_EQ(series.Recent[0].Time.TimeEnd, T1min);

  // modifiedTimes should contain the grid cell end time
  EXPECT_EQ(modified.size(), 1u);
  EXPECT_TRUE(modified.count(T1min));
  EXPECT_TRUE(removed.empty());
}

TEST(StatsData, FlushDistributesAcrossMultipleCells)
{
  CStatsSeries series(minutes(1), minutes(10));

  // Work spanning 2.5 minutes → should distribute across 3 grid cells (1min each)
  CWorkSummary work;
  work.SharesNum = 1000;
  work.SharesWork = UInt<256>(5000u);
  Timestamp t150s(150000);
  series.addWorkSummary(work, t150s);

  std::set<Timestamp> modified, removed;
  series.flush(Timestamp(0), t150s, "user1", "w1", t150s, nullptr, modified, removed);

  EXPECT_EQ(series.Current.SharesNum, 0u);
  ASSERT_EQ(series.Recent.size(), 3u);

  // Check grid boundaries
  EXPECT_EQ(series.Recent[0].Time.TimeEnd, T1min);
  EXPECT_EQ(series.Recent[1].Time.TimeEnd, T2min);
  EXPECT_EQ(series.Recent[2].Time.TimeEnd, T3min);

  // Total shares must be conserved across all cells
  uint64_t totalShares = 0;
  for (auto &cell : series.Recent)
    totalShares += cell.Data.SharesNum;
  EXPECT_EQ(totalShares, 1000u);

  EXPECT_EQ(modified.size(), 3u);
}

TEST(StatsData, FlushRemovesOldRecords)
{
  // keepTime=0min so removeTimePoint = timeLabel itself
  CStatsSeries series(minutes(1), minutes(0));

  // Pre-fill Recent with old data at [0..1min) and [1min..2min)
  for (int i = 0; i < 2; i++) {
    CWorkSummaryWithTime entry;
    entry.Time = TimeInterval(Timestamp(i * 60000), Timestamp((i + 1) * 60000));
    entry.Data.SharesNum = 50;
    entry.Data.SharesWork = UInt<256>(100u);
    series.Recent.push_back(entry);
  }
  ASSERT_EQ(series.Recent.size(), 2u);

  // Flush with no new data, timeLabel = 90s → removeTimePoint = 90s - 0 = 90s
  // First entry (TimeEnd = 60s < 90s) removed, second (TimeEnd = 120s >= 90s) stays
  Timestamp t90s(90000);
  std::set<Timestamp> modified, removed;
  series.flush(Timestamp(0), Timestamp(0), "user1", "w1", t90s, nullptr, modified, removed);

  ASSERT_EQ(series.Recent.size(), 1u);
  EXPECT_EQ(series.Recent[0].Time.TimeEnd, T2min);

  EXPECT_TRUE(modified.empty());
  EXPECT_EQ(removed.size(), 1u);
  EXPECT_TRUE(removed.count(T1min));
}

TEST(StatsData, FlushMergesIntoExistingRecent)
{
  CStatsSeries series(minutes(1), minutes(10));

  // Pre-fill Recent with data at [0..1min)
  CWorkSummaryWithTime existing;
  existing.Time = TimeInterval(Timestamp(0), T1min);
  existing.Data.SharesNum = 200;
  existing.Data.SharesWork = UInt<256>(500u);
  series.Recent.push_back(existing);

  // Add new work in the same grid cell
  CWorkSummary work;
  work.SharesNum = 100;
  work.SharesWork = UInt<256>(300u);
  series.addWorkSummary(work, Timestamp(30000));

  std::set<Timestamp> modified, removed;
  series.flush(Timestamp(0), T1min, "user1", "w1", T1min, nullptr, modified, removed);

  // Should merge, not create a second entry
  ASSERT_EQ(series.Recent.size(), 1u);
  EXPECT_EQ(series.Recent[0].Data.SharesNum, 300u);

  // Phase 2: inject a misaligned entry [5s, 55s) before the grid-aligned one
  CWorkSummaryWithTime misaligned;
  misaligned.Time = TimeInterval(Timestamp(5000), Timestamp(55000));
  misaligned.Data.SharesNum = 77;
  misaligned.Data.SharesWork = UInt<256>(777u);
  series.Recent.push_front(misaligned);
  // Recent: [{5000,55000}, {0,60000}]

  // Flush new work into [0, 60s) cell
  CWorkSummary work2;
  work2.SharesNum = 50;
  work2.SharesWork = UInt<256>(200u);
  series.addWorkSummary(work2, Timestamp(45000));

  modified.clear();
  removed.clear();
  series.flush(Timestamp(0), T1min, "user1", "w1", T1min, nullptr, modified, removed);

  // Misaligned entry untouched, grid-aligned entry got the merge
  ASSERT_EQ(series.Recent.size(), 2u);
  EXPECT_EQ(series.Recent[0].Data.SharesNum, 77u);
  EXPECT_EQ(series.Recent[0].Time.TimeEnd, Timestamp(55000));
  EXPECT_EQ(series.Recent[1].Data.SharesNum, 350u);
  EXPECT_EQ(series.Recent[1].Time.TimeEnd, T1min);
}

TEST(StatsData, FlushPointInTime)
{
  CStatsSeries series(minutes(1), minutes(10));

  // Add work at a single point in time (30 seconds)
  CWorkSummary work;
  work.SharesNum = 500;
  work.SharesWork = UInt<256>(2000u);
  Timestamp t30s(30000);
  series.addWorkSummary(work, t30s);

  // Flush with point interval (begin == end)
  std::set<Timestamp> modified, removed;
  series.flush(t30s, t30s, "user1", "w1", t30s, nullptr, modified, removed);

  // All work should land in the single cell containing the point [0, 60000)
  EXPECT_EQ(series.Current.SharesNum, 0u);
  ASSERT_EQ(series.Recent.size(), 1u);
  EXPECT_EQ(series.Recent[0].Data.SharesNum, 500u);
  EXPECT_EQ(series.Recent[0].Time.TimeBegin, Timestamp(0));
  EXPECT_EQ(series.Recent[0].Time.TimeEnd, T1min);

  EXPECT_EQ(modified.size(), 1u);
  EXPECT_TRUE(modified.count(T1min));
}

TEST(StatsData, FlushOldCellSkipsRecentWritesDb)
{
  // keepTime=0 so removeTimePoint = timeLabel = 5min
  // Work interval [0, 30s) → cell [0, 1min) has cellEnd=1min < removeTimePoint=5min
  // → should NOT appear in Recent, but batch should receive the record
  CStatsSeries series(minutes(1), minutes(0));

  CWorkSummary work;
  work.SharesNum = 100;
  work.SharesWork = UInt<256>(500u);
  series.addWorkSummary(work, Timestamp(30000));

  Timestamp t5min(300000);
  kvdb<rocksdbBase>::Batch batch;
  std::set<Timestamp> modified, removed;
  series.flush(Timestamp(0), Timestamp(30000), "user1", "w1", t5min, &batch, modified, removed);

  // Current must be reset
  EXPECT_EQ(series.Current.SharesNum, 0u);
  EXPECT_TRUE(series.Current.SharesWork.isZero());

  // Cell is too old for Recent
  EXPECT_TRUE(series.Recent.empty());

  // Not in modifiedTimes (no .dat rebuild for old data)
  EXPECT_TRUE(modified.empty());

  // Batch received the record (non-empty after flush)
  EXPECT_FALSE(batch.empty());
}

TEST(StatsData, FlushMixedOldAndNewCells)
{
  // keepTime=0 so removeTimePoint = timeLabel = 90s
  // Work interval [0, 120s) spans 2 cells:
  //   [0, 1min) → cellEnd=60s < 90s → old, skip Recent but write DB
  //   [1min, 2min) → cellEnd=120s >= 90s → goes to Recent
  CStatsSeries series(minutes(1), minutes(0));

  CWorkSummary work;
  work.SharesNum = 1000;
  work.SharesWork = UInt<256>(2000u);
  series.addWorkSummary(work, T2min);

  Timestamp t90s(90000);
  std::set<Timestamp> modified, removed;
  series.flush(Timestamp(0), T2min, "user1", "w1", t90s, nullptr, modified, removed);

  EXPECT_EQ(series.Current.SharesNum, 0u);

  // Only the second cell should be in Recent
  ASSERT_EQ(series.Recent.size(), 1u);
  EXPECT_EQ(series.Recent[0].Time.TimeEnd, T2min);
  // Half of shares (fraction = 0.5 for each cell)
  EXPECT_EQ(series.Recent[0].Data.SharesNum, 500u);

  // Only the new cell in modifiedTimes
  EXPECT_EQ(modified.size(), 1u);
  EXPECT_TRUE(modified.count(T2min));
}

TEST(StatsData, FlushWithEmptyCurrentOnlyRemoves)
{
  // keepTime=0 so removeTimePoint = timeLabel
  CStatsSeries series(minutes(1), minutes(0));

  // No Current data, some Recent
  CWorkSummaryWithTime entry;
  entry.Time = TimeInterval(Timestamp(0), T1min);
  entry.Data.SharesNum = 10;
  series.Recent.push_back(entry);

  std::set<Timestamp> modified, removed;
  series.flush(Timestamp(0), Timestamp(0), "", "", Timestamp(60001), nullptr, modified, removed);

  EXPECT_TRUE(series.Recent.empty());
  EXPECT_TRUE(modified.empty());
  EXPECT_EQ(removed.size(), 1u);
}
