#pragma once

#include "poolcommon/file.h"
#include "poolcommon/timeTypes.h"
#include <deque>
#include <filesystem>

struct CDatFile {
  std::filesystem::path Path;
  uint64_t FileId = 0;
  uint64_t LastShareId = 0;
  FileDescriptor Fd;
  unsigned Version = 0;
};

struct CFlushInfo {
  uint64_t ShareId;
  Timestamp Time;
};

void enumerateDatFiles(std::deque<CDatFile> &cache,
                       const std::filesystem::path &directory,
                       unsigned version,
                       bool createIfNotExists);
