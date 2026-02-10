#pragma once

#include "rocksdb/merge_operator.h"
#include <memory>

class StatsRecordMergeOperator : public rocksdb::AssociativeMergeOperator {
public:
  bool Merge(const rocksdb::Slice &key,
             const rocksdb::Slice *existing_value,
             const rocksdb::Slice &value,
             std::string *new_value,
             rocksdb::Logger *logger) const override;

  const char *Name() const override { return "StatsRecordMergeOperator"; }
};
