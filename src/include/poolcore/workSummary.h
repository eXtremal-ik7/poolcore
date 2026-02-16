#ifndef __WORK_SUMMARY_H_
#define __WORK_SUMMARY_H_

#include "poolcommon/serialize.h"
#include <cstdint>
#include <string>
#include <vector>

template<typename T> struct ShareLogIo;

struct CWorkSummaryWithTime;

struct CWorkSummary {
  uint64_t SharesNum = 0;
  UInt<256> SharesWork = UInt<256>::zero();
  uint32_t PrimePOWTarget = -1U;
  std::vector<uint64_t> PrimePOWSharesNum;

  void reset();
  void merge(const CWorkSummary &other);
  void mergeScaled(const CWorkSummary &other, double fraction);
};

struct CWorkSummaryWithTime {
  TimeInterval Time;
  CWorkSummary Data;
};

struct CWorkSummaryEntry {
  enum { CurrentRecordVersion = 1 };

  std::string UserId;
  std::string WorkerId;
  CWorkSummary Data;
};

struct CWorkSummaryBatch {
  TimeInterval Time;
  std::vector<CWorkSummaryEntry> Entries;
};

struct CUserWorkSummary {
  std::string UserId;
  UInt<256> AcceptedWork;
  uint64_t SharesNum = 0;
  Timestamp Time;
};

struct CUserWorkSummaryBatch {
  TimeInterval Time;
  std::vector<CUserWorkSummary> Entries;
};

// +serialization

template<>
struct DbIo<CWorkSummary> {
  static inline void serialize(xmstream &out, const CWorkSummary &data) {
    DbIo<decltype(data.SharesNum)>::serialize(out, data.SharesNum);
    DbIo<decltype(data.SharesWork)>::serialize(out, data.SharesWork);
    DbIo<decltype(data.PrimePOWTarget)>::serialize(out, data.PrimePOWTarget);
    DbIo<decltype(data.PrimePOWSharesNum)>::serialize(out, data.PrimePOWSharesNum);
  }

  static inline void unserialize(xmstream &in, CWorkSummary &data) {
    DbIo<decltype(data.SharesNum)>::unserialize(in, data.SharesNum);
    DbIo<decltype(data.SharesWork)>::unserialize(in, data.SharesWork);
    DbIo<decltype(data.PrimePOWTarget)>::unserialize(in, data.PrimePOWTarget);
    DbIo<decltype(data.PrimePOWSharesNum)>::unserialize(in, data.PrimePOWSharesNum);
  }
};

template<>
struct DbIo<CWorkSummaryEntry> {
  static inline void serialize(xmstream &out, const CWorkSummaryEntry &data) {
    DbIo<uint32_t>::serialize(out, data.CurrentRecordVersion);
    DbIo<decltype(data.UserId)>::serialize(out, data.UserId);
    DbIo<decltype(data.WorkerId)>::serialize(out, data.WorkerId);
    DbIo<decltype(data.Data)>::serialize(out, data.Data);
  }

  static inline void unserialize(xmstream &in, CWorkSummaryEntry &data) {
    uint32_t version;
    DbIo<uint32_t>::unserialize(in, version);
    if (version == 1) {
      DbIo<decltype(data.UserId)>::unserialize(in, data.UserId);
      DbIo<decltype(data.WorkerId)>::unserialize(in, data.WorkerId);
      DbIo<decltype(data.Data)>::unserialize(in, data.Data);
    } else {
      // Unknown version — skip the rest of the file; remaining records are
      // discarded intentionally since we cannot parse them reliably
      in.seekEnd(0, true);
    }
  }
};

template<>
struct DbIo<CUserWorkSummary> {
  static inline void serialize(xmstream &stream, const CUserWorkSummary &data) {
    DbIo<decltype(data.UserId)>::serialize(stream, data.UserId);
    DbIo<decltype(data.AcceptedWork)>::serialize(stream, data.AcceptedWork);
    DbIo<decltype(data.SharesNum)>::serialize(stream, data.SharesNum);
    DbIo<decltype(data.Time)>::serialize(stream, data.Time);
  }

  static inline void unserialize(xmstream &stream, CUserWorkSummary &data) {
    DbIo<decltype(data.UserId)>::unserialize(stream, data.UserId);
    DbIo<decltype(data.AcceptedWork)>::unserialize(stream, data.AcceptedWork);
    DbIo<decltype(data.SharesNum)>::unserialize(stream, data.SharesNum);
    DbIo<decltype(data.Time)>::unserialize(stream, data.Time);
  }
};

// +share log serialization

template<>
struct ShareLogIo<CWorkSummaryBatch> {
  static inline void serialize(xmstream &out, const CWorkSummaryBatch &data) {
    DbIo<TimeInterval>::serialize(out, data.Time);
    DbIo<uint32_t>::serialize(out, static_cast<uint32_t>(data.Entries.size()));
    for (const auto &entry : data.Entries)
      DbIo<CWorkSummaryEntry>::serialize(out, entry);
  }

  static inline void unserialize(xmstream &in, CWorkSummaryBatch &data) {
    DbIo<TimeInterval>::unserialize(in, data.Time);
    uint32_t count;
    DbIo<uint32_t>::unserialize(in, count);
    data.Entries.resize(count);
    for (auto &entry : data.Entries)
      DbIo<CWorkSummaryEntry>::unserialize(in, entry);
  }
};

template<>
struct ShareLogIo<CUserWorkSummaryBatch> {
  static inline void serialize(xmstream &out, const CUserWorkSummaryBatch &data) {
    DbIo<TimeInterval>::serialize(out, data.Time);
    DbIo<uint32_t>::serialize(out, static_cast<uint32_t>(data.Entries.size()));
    for (const auto &entry : data.Entries)
      DbIo<CUserWorkSummary>::serialize(out, entry);
  }

  static inline void unserialize(xmstream &in, CUserWorkSummaryBatch &data) {
    DbIo<TimeInterval>::unserialize(in, data.Time);
    uint32_t count;
    DbIo<uint32_t>::unserialize(in, count);
    data.Entries.resize(count);
    for (auto &entry : data.Entries)
      DbIo<CUserWorkSummary>::unserialize(in, entry);
  }
};

#endif //__WORK_SUMMARY_H_
