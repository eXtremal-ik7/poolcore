#include "poolcore/shareLog.h"

void ShareLogIo<CShare>::serialize(xmstream &out, const CShare &data)
{
  out.writele<uint32_t>(data.CurrentRecordVersion);
  out.writele<uint64_t>(data.UniqueShareId);
  DbIo<std::string>::serialize(out, data.userId);
  DbIo<std::string>::serialize(out, data.workerId);
  out.write<double>(data.WorkValue);
  DbIo<int64_t>::serialize(out, data.Time);
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
  data.WorkValue = out.read<double>();
  DbIo<int64_t>::unserialize(out, data.Time);
  DbIo<uint32_t>::unserialize(out, data.ChainLength);
  DbIo<uint32_t>::unserialize(out, data.PrimePOWTarget);
}
