#ifndef __KVDB_H_
#define __KVDB_H_

#include "leveldb/db.h"
#include "p2putils/coreTypes.h"
#include "p2putils/xmstream.h"






template<typename DbTy>
class kvdb {
private:
  DbTy _db;
  
public:
  kvdb(const std::string &path) : _db(path) {}
  
  template<typename D>
  void put(const D &data) {
    xmstream stream;
    data.serializeKey(stream);
    size_t keySize = stream.offsetOf();
    data.serializeValue(stream);
    size_t valueSize = stream.offsetOf() - keySize;
    
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
/*  
  template<typename D> static void iteratorSeek(typename DbTy::IteratorType *It, const D &data) {
    uint8_t buffer[128];
    xmstream S(buffer, sizeof(buffer));
    data.serializeKey();
    RawData key;
    key.data = S.data<uint8_t>();
    key.size = S.sizeOf();
    It->seek(data.getPartitionId(), key);
  }*/
  
  void clear() { _db.clear(); }
};

#endif //__KVDB_H_
