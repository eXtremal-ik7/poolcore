#ifndef __KVDB_H_
#define __KVDB_H_

#include "p2putils/coreTypes.h"
#include "p2putils/xmstream.h"
#include <filesystem>
#include <vector>

template<typename DbTy>
class kvdb {
private:
  DbTy _db;

public:
  kvdb(const std::filesystem::path &path) : _db(path) {}

  // Multi-partition batch. Accumulates put/merge/deleteRow operations across partitions.
  // Most efficient when consecutive calls target the same partition —
  // each partition switch starts a new internal WriteBatch.
  // If writes alternate between partitions on every call, no batching benefit is achieved.
  class Batch {
    std::vector<typename DbTy::CBatch> Batches_;
    std::string CurrentPartitionId_;

    typename DbTy::CBatch &getBatch(const std::string &partitionId) {
      if (CurrentPartitionId_ != partitionId) {
        Batches_.emplace_back();
        Batches_.back().PartitionId = partitionId;
        CurrentPartitionId_ = partitionId;
      }
      return Batches_.back();
    }

    friend class kvdb;

  public:
    template<typename D>
    void put(const D &data) {
      xmstream stream;
      data.serializeKey(stream);
      size_t keySize = stream.offsetOf();
      data.serializeValue(stream);
      const uint8_t *keyData = (const uint8_t*)stream.data();
      getBatch(data.getPartitionId()).put(keyData, keySize, keyData+keySize, stream.sizeOf()-keySize);
    }

    template<typename D>
    void merge(const D &data) {
      xmstream stream;
      data.serializeKey(stream);
      size_t keySize = stream.offsetOf();
      data.serializeValue(stream);
      const uint8_t *keyData = (const uint8_t*)stream.data();
      getBatch(data.getPartitionId()).merge(keyData, keySize, keyData+keySize, stream.sizeOf()-keySize);
    }

    template<typename D>
    void deleteRow(const D &data) {
      xmstream stream;
      data.serializeKey(stream);
      getBatch(data.getPartitionId()).deleteRow((const uint8_t*)stream.data(), stream.sizeOf());
    }
  };

  template<typename D>
  void put(const D &data) {
    xmstream stream;
    data.serializeKey(stream);
    size_t keySize = stream.offsetOf();
    data.serializeValue(stream);

    const uint8_t *keyData = (const uint8_t*)stream.data();
    _db.put(data.getPartitionId(), keyData, keySize, keyData+keySize, stream.sizeOf()-keySize);
  }

  template<typename D>
  void deleteRow(const D &data) {
    xmstream stream;
    data.serializeKey(stream);
    _db.deleteRow(data.getPartitionId(), (const uint8_t*)stream.data(), stream.sizeOf());
  }

  typename DbTy::IteratorType *iterator() { return _db.iterator(); }

  void writeBatch(Batch &batch) {
    for (auto &b : batch.Batches_)
      _db.writeBatch(b);
  }

  void clear() { _db.clear(); }
};

#endif //__KVDB_H_
