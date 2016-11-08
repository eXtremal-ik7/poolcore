#ifndef __LEVELDB_BASE_H_
#define __LEVELDB_BASE_H_

#include "p2putils/coreTypes.h"
#include "p2putils/xmstream.h"
#include <boost/filesystem.hpp>
#include "leveldb/db.h"

#include <string>
#include <vector>

namespace leveldb {
  class DB;
  class Iterator;
}


class levelDbBase {
public:
  struct IteratorType {
    levelDbBase *base;
    std::string id;
    leveldb::Iterator *iterator;
    bool end;
    
    void cleanup() { delete iterator; iterator = 0; }
    
    IteratorType(levelDbBase *baseArg) : base(baseArg), id(), iterator(0), end(false) {}
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
        
        leveldb::ReadOptions options;
        id = p.id;
        iterator = p.db->NewIterator(options);
      }
      
      char buffer[128];
      xmstream S(buffer, sizeof(buffer));
      S.reset();
      data.serializeKey(S);
      leveldb::Slice slice(S.data<const char>(), S.sizeOf());
      iterator->Seek(slice);
      while (!iterator->Valid()) {
        cleanup();
        auto np = base->greaterPartition(id);
        if (!np.db) {
          end = true;
          return;
        }
          
        leveldb::ReadOptions options;
        id = np.id;
        iterator = np.db->NewIterator(options);
        iterator->SeekToFirst();
      }
    }
  };
  
private:
  struct partition {
    std::string id;
    leveldb::DB *db;
    partition() : id(), db(0) {}
    partition(const std::string &idArg) : id(idArg), db(0) {}
    friend bool operator<(const partition &l, const partition &r) { return l.id < r.id; }
  };
  
private:
  boost::filesystem::path _path;
  std::vector<partition> _partitions;
  
  partition getFirstPartition();
  partition getLastPartition();
  partition lessPartition(const std::string &id);  
  partition greaterPartition(const std::string &id);
  partition greaterOrEqualPartition(const std::string &id);  
  
  leveldb::DB *open(partition &partition);
  leveldb::DB *getPartition(const std::string &id);
  leveldb::DB *getOrCreatePartition(const std::string &id);
  
  
public:
  levelDbBase(const std::string &path);
  ~levelDbBase();
  
  bool put(const std::string &partitionId, const void *key, size_t keySize, const void *data, size_t dataSize);
  bool deleteRow(const std::string &partitionId, const void *key, size_t keySize);
  void clear();
  
  IteratorType *iterator();
};

#endif //__LEVELDB_BASE_H_
