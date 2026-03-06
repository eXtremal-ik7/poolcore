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
