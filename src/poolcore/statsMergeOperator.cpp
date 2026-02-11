#include "poolcore/statsMergeOperator.h"
#include "poolcore/statsData.h"
#include "p2putils/xmstream.h"

bool StatsRecordMergeOperator::Merge(const rocksdb::Slice &key,
                                     const rocksdb::Slice *existing_value,
                                     const rocksdb::Slice &value,
                                     std::string *new_value,
                                     rocksdb::Logger *logger) const
{
  if (!existing_value) {
    new_value->assign(value.data(), value.size());
    return true;
  }

  StatsRecord existing;
  if (!existing.deserializeValue(existing_value->data(), existing_value->size()))
    return false;

  StatsRecord incoming;
  if (!incoming.deserializeValue(value.data(), value.size()))
    return false;

  // Merge following CStatsSeries::addShare pattern
  existing.ShareCount += incoming.ShareCount;
  existing.ShareWork += incoming.ShareWork;
  existing.PrimePOWTarget = std::min(existing.PrimePOWTarget, incoming.PrimePOWTarget);

  if (incoming.PrimePOWShareCount.size() > existing.PrimePOWShareCount.size())
    existing.PrimePOWShareCount.resize(incoming.PrimePOWShareCount.size());
  for (size_t i = 0; i < incoming.PrimePOWShareCount.size(); i++)
    existing.PrimePOWShareCount[i] += incoming.PrimePOWShareCount[i];

  existing.Time.TimeBegin = std::min(existing.Time.TimeBegin, incoming.Time.TimeBegin);
  // TimeEnd stays the same (grid-aligned, same key)
  existing.UpdateTime = std::max(existing.UpdateTime, incoming.UpdateTime);

  xmstream stream;
  existing.serializeValue(stream);
  new_value->assign(stream.data<const char>(), stream.sizeOf());
  return true;
}
