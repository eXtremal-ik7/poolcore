#pragma once

#include "backendData.h"
#include "workSummary.h"
#include <optional>

inline std::string ppsMetaUserId() { return "pps.meta\x01"; }

enum class ESaturationFunction : uint32_t {
  None = 0,
  Tangent,
  Clamp,
  Cubic,
  Softsign,
  Norm,
  Atan,
  Exp,
};

struct CBackendSettings {

public:
  struct PPS {

  public:
    static bool parseSaturationFunction(const std::string &name, ESaturationFunction *out);
    static const char *saturationFunctionName(ESaturationFunction value);
    static const std::vector<const char*> &saturationFunctionNames();
    double saturateCoeff(double balanceInBlocks) const;

    bool Enabled = false;
    double PoolFee = 4.0;
    ESaturationFunction SaturationFunction = ESaturationFunction::None;
    double SaturationB0 = 0.0;
    double SaturationANegative = 0.0;
    double SaturationAPositive = 0.0;
  };

  struct Payouts {

  public:
    bool InstantPayoutsEnabled = true;
    bool RegularPayoutsEnabled = true;
    // Instant payouts: transaction fee is deducted from user's balance
    UInt<384> InstantMinimalPayout;
    std::chrono::minutes InstantPayoutInterval = std::chrono::minutes(1);
    // Regular payouts: transaction fee is deducted from pool's balance
    UInt<384> RegularMinimalPayout;
    std::chrono::hours RegularPayoutInterval = std::chrono::hours(24);
    std::chrono::hours RegularPayoutDayOffset = std::chrono::hours(0);
  };

  PPS PPSConfig;
  Payouts PayoutConfig;
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

template<> struct DbIo<CBackendSettings> {
  static inline void serialize(xmstream &dst, const CBackendSettings &data) {
    // PPS
    DbIo<bool>::serialize(dst, data.PPSConfig.Enabled);
    DbIo<double>::serialize(dst, data.PPSConfig.PoolFee);
    DbIo<uint32_t>::serialize(dst, static_cast<uint32_t>(data.PPSConfig.SaturationFunction));
    DbIo<double>::serialize(dst, data.PPSConfig.SaturationB0);
    DbIo<double>::serialize(dst, data.PPSConfig.SaturationANegative);
    DbIo<double>::serialize(dst, data.PPSConfig.SaturationAPositive);
    // Payouts
    DbIo<bool>::serialize(dst, data.PayoutConfig.InstantPayoutsEnabled);
    DbIo<bool>::serialize(dst, data.PayoutConfig.RegularPayoutsEnabled);
    DbIo<UInt<384>>::serialize(dst, data.PayoutConfig.InstantMinimalPayout);
    DbIo<std::chrono::minutes>::serialize(dst, data.PayoutConfig.InstantPayoutInterval);
    DbIo<UInt<384>>::serialize(dst, data.PayoutConfig.RegularMinimalPayout);
    DbIo<std::chrono::hours>::serialize(dst, data.PayoutConfig.RegularPayoutInterval);
    DbIo<std::chrono::hours>::serialize(dst, data.PayoutConfig.RegularPayoutDayOffset);
  }

  static inline void unserialize(xmstream &src, CBackendSettings &data) {
    // PPS
    DbIo<bool>::unserialize(src, data.PPSConfig.Enabled);
    DbIo<double>::unserialize(src, data.PPSConfig.PoolFee);
    uint32_t saturationFunction = 0;
    DbIo<uint32_t>::unserialize(src, saturationFunction);
    data.PPSConfig.SaturationFunction = static_cast<ESaturationFunction>(saturationFunction);
    DbIo<double>::unserialize(src, data.PPSConfig.SaturationB0);
    DbIo<double>::unserialize(src, data.PPSConfig.SaturationANegative);
    DbIo<double>::unserialize(src, data.PPSConfig.SaturationAPositive);
    // Payouts
    DbIo<bool>::unserialize(src, data.PayoutConfig.InstantPayoutsEnabled);
    DbIo<bool>::unserialize(src, data.PayoutConfig.RegularPayoutsEnabled);
    DbIo<UInt<384>>::unserialize(src, data.PayoutConfig.InstantMinimalPayout);
    DbIo<std::chrono::minutes>::unserialize(src, data.PayoutConfig.InstantPayoutInterval);
    DbIo<UInt<384>>::unserialize(src, data.PayoutConfig.RegularMinimalPayout);
    DbIo<std::chrono::hours>::unserialize(src, data.PayoutConfig.RegularPayoutInterval);
    DbIo<std::chrono::hours>::unserialize(src, data.PayoutConfig.RegularPayoutDayOffset);
  }
};

template<> struct DbIo<CPPSBalanceSnapshot> {
  static void serialize(xmstream &dst, const CPPSBalanceSnapshot &data);
  static void unserialize(xmstream &src, CPPSBalanceSnapshot &data);
};

template<> struct DbIo<CPPSState> {
  static void serialize(xmstream &dst, const CPPSState &data);
  static void unserialize(xmstream &src, CPPSState &data);
};
