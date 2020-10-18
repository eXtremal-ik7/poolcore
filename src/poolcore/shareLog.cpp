#include "poolcore/shareLog.h"

void ShareLogIo<CShare>::serialize(xmstream &out, const CShare &data)
{
  out.writele<uint64_t>(data.UniqueShareId);
  DbIo<std::string>::serialize(out, data.userId);
  DbIo<std::string>::serialize(out, data.workerId);
  out.write<double>(data.WorkValue);
  DbIo<int64_t>::serialize(out, data.Time);
}

void ShareLogIo<CShare>::unserialize(xmstream &out, CShare &data)
{
  data.UniqueShareId = out.readle<uint64_t>();
  DbIo<std::string>::unserialize(out, data.userId);
  DbIo<std::string>::unserialize(out, data.workerId);
  data.WorkValue = out.read<double>();
  DbIo<int64_t>::unserialize(out, data.Time);
}
