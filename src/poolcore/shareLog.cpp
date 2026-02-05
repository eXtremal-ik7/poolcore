#include "poolcore/shareLog.h"

void ShareLogIo<CShare>::serialize(xmstream &out, const CShare &data)
{
  out.writele<uint32_t>(data.CurrentRecordVersion);
  out.writele<uint64_t>(data.UniqueShareId);
  DbIo<std::string>::serialize(out, data.userId);
  DbIo<std::string>::serialize(out, data.workerId);
  DbIo<UInt<256>>::serialize(out, data.WorkValue);
  DbIo<int64_t>::serialize(out, data.Time.toUnixTime());
  DbIo<uint32_t>::serialize(out, data.ChainLength);
  DbIo<uint32_t>::serialize(out, data.PrimePOWTarget);
}

void ShareLogIo<CShare>::unserialize(xmstream &out, CShare &data)
{
  uint32_t version;
  version = out.readle<uint32_t>();
  data.UniqueShareId = out.readle<uint64_t>();
  DbIo<std::string>::unserialize(out, data.userId);
  DbIo<std::string>::unserialize(out, data.workerId);
  DbIo<UInt<256>>::unserialize(out, data.WorkValue);
  {
    int64_t timeSeconds;
    DbIo<int64_t>::unserialize(out, timeSeconds);
    data.Time = Timestamp::fromUnixTime(timeSeconds);
  }
  DbIo<uint32_t>::unserialize(out, data.ChainLength);
  DbIo<uint32_t>::unserialize(out, data.PrimePOWTarget);
}
