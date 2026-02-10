#ifndef __LEVELDB_BASE_H_
#define __LEVELDB_BASE_H_

#include "p2putils/coreTypes.h"
#include "p2putils/xmstream.h"
#include "rocksdb/db.h"
#include "rocksdb/merge_operator.h"

#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace leveldb {
  class DB;
  class Iterator;
}


class rocksdbBase {
public:
  struct CBatch {
    std::string PartitionId;
    rocksdb::WriteBatch Batch;

    bool put(const void *key, size_t keySize, const void *data, size_t dataSize);
    bool merge(const void *key, size_t keySize, const void *data, size_t dataSize);
    bool deleteRow(const void *key, size_t keySize);
  };

  struct IteratorType {
    rocksdbBase *base;
    std::string id;
    rocksdb::Iterator *iterator;
    bool end;
    
    void cleanup() { delete iterator; iterator = 0; }
    
    IteratorType(rocksdbBase *baseArg) : base(baseArg), id(), iterator(0), end(false) {}
    ~IteratorType();
    bool valid();
    void prev();
    void next();
    void seekFirst();
    void seekLast();
    RawData key();
    RawData value();
    
    template<typename D> void seek(const D &data) {
      std::string partId = data.getPartitionId();
      if (id != partId) {
        cleanup();
        auto p = base->greaterOrEqualPartition(partId);
        if (!p.db) {
          end = true;
          return;
        }
        
        rocksdb::ReadOptions options;
        id = p.id;
        iterator = p.db->NewIterator(options);
      }
      
      char buffer[128];
      xmstream S(buffer, sizeof(buffer));
      S.reset();
      data.serializeKey(S);
      rocksdb::Slice slice(S.data<const char>(), S.sizeOf());
      iterator->Seek(slice);
      while (!iterator->Valid()) {
        cleanup();
        auto np = base->greaterPartition(id);
        if (!np.db) {
          end = true;
          return;
        }
          
        rocksdb::ReadOptions options;
        id = np.id;
        iterator = np.db->NewIterator(options);
        iterator->SeekToFirst();
      }
    }

    template<typename D> void seekForPrev(const D &data) {
      std::string partId = data.getPartitionId();
      if (id != partId) {
        cleanup();
        auto p = base->lessOrEqualPartition(partId);
        if (!p.db) {
          end = true;
          return;
        }

        rocksdb::ReadOptions options;
        id = p.id;
        iterator = p.db->NewIterator(options);
      }

      char buffer[128];
      xmstream S(buffer, sizeof(buffer));
      S.reset();
      data.serializeKey(S);
      rocksdb::Slice slice(S.data<const char>(), S.sizeOf());
      iterator->SeekForPrev(slice);
      while (!iterator->Valid()) {
        cleanup();
        auto np = base->lessPartition(id);
        if (!np.db) {
          end = true;
          return;
        }

        rocksdb::ReadOptions options;
        id = np.id;
        iterator = np.db->NewIterator(options);
        iterator->SeekToLast();
      }
    }


    template<typename ValueType> void seekForPrev(const ValueType &firstKey, const void *nextKeyData, size_t nextKeySize, ValueType &out, const std::function<bool(const ValueType&)> &validPredicate) {
      std::string partId = firstKey.getPartitionId();
      if (id != partId) {
        cleanup();
        auto p = base->lessOrEqualPartition(partId);
        if (!p.db)
          return;

        id = p.id;
        rocksdb::ReadOptions options;
        iterator = p.db->NewIterator(options);
      }

      char buffer[128];
      xmstream S(buffer, sizeof(buffer));
      S.reset();
      firstKey.serializeKey(S);
      rocksdb::Slice firstKeySlice(S.data<const char>(), S.sizeOf());
      iterator->SeekForPrev(firstKeySlice);

      rocksdb::Slice nextKeySlice(static_cast<const char*>(nextKeyData), nextKeySize);
      while (!checkValid(out, validPredicate)) {
        cleanup();
        auto np = base->lessPartition(id);
        if (!np.db)
          return;

        id = np.id;
        rocksdb::ReadOptions options;
        iterator = np.db->NewIterator(options);
        iterator->SeekForPrev(nextKeySlice);
      }
    }

    template<typename ValueType> void prev(const void *nextKeyData, size_t nextKeySize, ValueType &out, const std::function<bool(const ValueType&)> &validPredicate) {
      if (iterator)
        iterator->Prev();

      rocksdb::ReadOptions options;
      rocksdb::Slice nextKeySlice(static_cast<const char*>(nextKeyData), nextKeySize);
      while (!checkValid(out, validPredicate)) {
        if (id.empty())
          return;

        cleanup();
        auto p = base->lessPartition(id);
        if (!p.db)
          return;

        id = p.id;

        iterator = p.db->NewIterator(options);
        iterator->SeekForPrev(nextKeySlice);
      }
    }

  private:
    template<typename ValueType> bool checkValid(ValueType &out, const std::function<bool(const ValueType&)> &validPredicate) {
      if (!iterator->Valid())
        return false;
      if (!out.deserializeValue(iterator->value().data(), iterator->value().size()))
        return false;
      if (!validPredicate(out))
        return false;
      return true;
    }
  };
  
private:
  struct partition {
    std::string id;
    rocksdb::DB *db;
    partition() : id(), db(nullptr) {}
    partition(const std::string &idArg) : id(idArg), db(0) {}
    friend bool operator<(const partition &l, const partition &r) { return l.id < r.id; }
  };
  
private:
  std::filesystem::path _path;
  std::vector<partition> _partitions;
  std::shared_mutex PartitionsMutex_;
  std::mutex DbMutex_;
  std::shared_ptr<rocksdb::MergeOperator> MergeOperator_;
  
  partition getFirstPartition();
  partition getLastPartition();
  partition lessPartition(const std::string &id);
  partition lessOrEqualPartition(const std::string &id);
  partition greaterPartition(const std::string &id);
  partition greaterOrEqualPartition(const std::string &id);
  
  rocksdb::DB *open(partition &partition);
  rocksdb::DB *getPartition(const std::string &id);
  rocksdb::DB *getOrCreatePartition(const std::string &id);
  
  
public:
  rocksdbBase(const std::filesystem::path &path);
  rocksdbBase(const std::filesystem::path &path, std::shared_ptr<rocksdb::MergeOperator> mergeOp);
  ~rocksdbBase();
  
  bool put(const std::string &partitionId, const void *key, size_t keySize, const void *data, size_t dataSize);
  bool deleteRow(const std::string &partitionId, const void *key, size_t keySize);
  void clear();
  
  IteratorType *iterator();

  CBatch batch(const std::string &partitionId);
  bool writeBatch(CBatch &batch);
};

#endif //__LEVELDB_BASE_H_
