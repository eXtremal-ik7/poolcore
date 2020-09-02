#ifndef __LEVELDB_BASE_H_
#define __LEVELDB_BASE_H_

#include "p2putils/coreTypes.h"
#include "p2putils/xmstream.h"
#include "rocksdb/db.h"

#include <filesystem>
#include <string>
#include <vector>

namespace leveldb {
  class DB;
  class Iterator;
}


class rocksdbBase {
public:
  struct PartitionBatchType {
    std::string PartitionId;
    rocksdb::WriteBatch Batch;

    bool put(const void *key, size_t keySize, const void *data, size_t dataSize);
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
    void prev(std::function<bool(const void *data, size_t)> endPredicate, const void *resumeKey, size_t resumeKeySize);
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
  };
  
private:
  struct partition {
    std::string id;
    rocksdb::DB *db;
    partition() : id(), db(0) {}
    partition(const std::string &idArg) : id(idArg), db(0) {}
    friend bool operator<(const partition &l, const partition &r) { return l.id < r.id; }
  };
  
private:
  std::filesystem::path _path;
  std::vector<partition> _partitions;
  
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
  ~rocksdbBase();
  
  bool put(const std::string &partitionId, const void *key, size_t keySize, const void *data, size_t dataSize);
  bool deleteRow(const std::string &partitionId, const void *key, size_t keySize);
  void clear();
  
  IteratorType *iterator();

  PartitionBatchType batch(const std::string &partitionId);
  bool writeBatch(PartitionBatchType &batch);
};

#endif //__LEVELDB_BASE_H_
