find_library(LEVELDB_LIBRARY leveldb
  PATH ${ROOT_SOURCE_DIR}/leveldb-1.18
)

find_path(LEVELDB_INCLUDE_DIR "leveldb/db.h"
  PATH ${ROOT_SOURCE_DIR}/leveldb-1.18/include
)
