#include "poolcore/statsMergeOperator.h"
#include "poolcore/statsData.h"
#include "p2putils/xmstream.h"

bool CWorkSummaryMergeOperator::Merge(const rocksdb::Slice &key,
                                      const rocksdb::Slice *existing_value,
                                      const rocksdb::Slice &value,
                                      std::string *new_value,
                                      rocksdb::Logger *logger) const
{
  if (!existing_value) {
    new_value->assign(value.data(), value.size());
    return true;
  }

  CWorkSummaryEntryWithTime existing;
  if (!existing.deserializeValue(existing_value->data(), existing_value->size()))
    return false;

  CWorkSummaryEntryWithTime incoming;
  if (!incoming.deserializeValue(value.data(), value.size()))
    return false;

  existing.Data.merge(incoming.Data);
  existing.Time.TimeBegin = std::min(existing.Time.TimeBegin, incoming.Time.TimeBegin);

  xmstream stream;
  existing.serializeValue(stream);
  new_value->assign(stream.data<const char>(), stream.sizeOf());
  return true;
}
