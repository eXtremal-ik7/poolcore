#ifndef __KVDB_H_
#define __KVDB_H_

#include "p2putils/coreTypes.h"
#include "p2putils/xmstream.h"
#include <filesystem>

template<typename DbTy>
class kvdb {
private:
  DbTy _db;
  
public:
  kvdb(const std::filesystem::path &path) : _db(path) {}
  
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
  void put(typename DbTy::PartitionBatchType &batch, const D &data) {
    xmstream stream;
    data.serializeKey(stream);
    size_t keySize = stream.offsetOf();
    data.serializeValue(stream);
    size_t valueSize = stream.offsetOf() - keySize;

    const uint8_t *keyData = (const uint8_t*)stream.data();
    batch.put(keyData, keySize, keyData+keySize, stream.sizeOf()-keySize);
  }
  
  template<typename D>  
  void deleteRow(const D &data) {
    xmstream stream;
    data.serializeKey(stream);
    _db.deleteRow(data.getPartitionId(), (const uint8_t*)stream.data(), stream.sizeOf());
  }

  template<typename D>
  void deleteRow(typename DbTy::PartitionBatchType &batch, const D &data) {
    xmstream stream;
    data.serializeKey(stream);
    batch.deleteRow((const uint8_t*)stream.data(), stream.sizeOf());
  }
  
  typename DbTy::IteratorType *iterator() { return _db.iterator(); }
  typename DbTy::PartitionBatchType batch(const std::string partitionId) { return _db.batch(partitionId); }
  void writeBatch(typename DbTy::PartitionBatchType &batch) { _db.writeBatch(batch); }
  void clear() { _db.clear(); }
};

#endif //__KVDB_H_
