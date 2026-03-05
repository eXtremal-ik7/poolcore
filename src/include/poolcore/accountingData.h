#pragma once

#include "backendData.h"
#include "poolcommon/tagged.h"
#include "workSummary.h"
#include <optional>

inline std::string ppsMetaUserId() { return "pps.meta\x01"; }

double saturateCoeff(const CBackendPPS &pps, double balanceInBlocks);

struct CBackendSettings {

public:
  CBackendPPS PPSConfig;
  CBackendPayouts PayoutConfig;
  CBackendSwap SwapConfig;

  static constexpr auto schema() {
    return std::make_tuple(
      field<1, &CBackendSettings::PPSConfig>(),
      field<2, &CBackendSettings::PayoutConfig>(),
      field<3, &CBackendSettings::SwapConfig>()
    );
  }
};

struct CPPSBalanceSnapshot {

public:
  UInt<384> Balance;
  double TotalBlocksFound = 0.0;
  Timestamp Time;
};

struct CPPSState {

public:
  // Pool-side PPS balance: increases when block found (PPS correction from PPLNS),
  // decreases when PPS rewards are accrued to users.
  // Can go negative — that's the pool's risk.
  UInt<384> Balance;
  // Reference PPS balance: tracks pure PPS risk without pool fee profit.
  // Increases by full block reward on block found, decreases by full share cost
  // (before pool fee deduction). Used for saturation coefficient and min/max tracking.
  UInt<384> ReferenceBalance;
  // Base reward of the last known block (subsidy without tx fees, fixed-point 128.256)
  UInt<384> LastBaseBlockReward;
  // Fractional count of blocks found (only PPS portion of each block)
  double TotalBlocksFound = 0.0;
  // Fractional count of orphan blocks (PPS portion)
  double OrphanBlocks = 0.0;

  CPPSBalanceSnapshot Min;
  CPPSBalanceSnapshot Max;

  // Last applied saturation coefficient (1.0 = no correction)
  double LastSaturateCoeff = 1.0;
  // Last average transaction fee per block (fixed-point 128.256)
  UInt<384> LastAverageTxFee;

  // Timestamp of this snapshot (used as kvdb key for history)
  Timestamp Time;

  static double balanceInBlocks(const UInt<384> &balance, const UInt<384> &baseBlockReward);
  static double sqLambda(
    const UInt<384> &balance,
    const UInt<384> &baseBlockReward,
    double totalBlocksFound);
  void updateMinMax(Timestamp now);

  std::string getPartitionId() const { return partByTime(Time.toUnixTime()); }
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
  bool deserializeValue(const void *data, size_t size);
};

struct CRewardParams {
  double RateToBTC = 0;
  double RateBTCToUSD = 0;
  // PPLNS fields
  Timestamp RoundStartTime;
  Timestamp RoundEndTime;
  std::string BlockHash;
  uint64_t BlockHeight = 0;
  // PPS fields
  Timestamp IntervalBegin;
  Timestamp IntervalEnd;
};

struct CAccountingStateBatch {
  double LastSaturateCoeff = 1.0;
  UInt<384> LastBaseBlockReward;
  UInt<384> LastAverageTxFee;
  // Full cost of PPS shares before any fee deduction (for ReferenceBalance tracking)
  UInt<384> PPSReferenceCost;
  std::vector<std::pair<std::string, UInt<256>>> PPLNSScores;
  std::vector<std::pair<std::string, UInt<384>>> PPSBalances;
};

struct CProcessedWorkSummary {
  CAccountingStateBatch AccountingBatch;
  CUserWorkSummaryBatch StatsBatch;
};

// DbIo specializations

template<> struct DbIo<CPPSBalanceSnapshot> {
  static void serialize(xmstream &dst, const CPPSBalanceSnapshot &data);
  static void unserialize(xmstream &src, CPPSBalanceSnapshot &data);
};

template<> struct DbIo<CPPSState> {
  static void serialize(xmstream &dst, const CPPSState &data);
  static void unserialize(xmstream &src, CPPSState &data);
};
