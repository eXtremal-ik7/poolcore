#include "poolcommon/datFile.h"
#include "loguru.hpp"
#include <algorithm>
#include <p2putils/strExtras.h>

void enumerateDatFiles(std::deque<CDatFile> &cache,
                       const std::filesystem::path &directory,
                       unsigned version,
                       bool createIfNotExists)
{
  std::error_code errc;
  if (createIfNotExists) {
    std::filesystem::create_directories(directory, errc);
  } else if (!std::filesystem::exists(directory)) {
    return;
  }

  for (std::filesystem::directory_iterator I(directory), IE; I != IE; ++I) {
    std::string fileName = I->path().filename().generic_string();
    auto dotDatPos = fileName.find(".dat");
    if (dotDatPos == fileName.npos) {
      CLOG_F(WARNING, "Unexpected file in dat directory: {}", fileName);
      continue;
    }

    fileName.resize(dotDatPos);

    auto &file = cache.emplace_back();
    file.Path = *I;
    file.FileId = xatoi<uint64_t>(fileName.c_str());
    file.Version = version;
  }

  std::sort(cache.begin(), cache.end(), [](const CDatFile &l, const CDatFile &r) {
    return l.FileId < r.FileId;
  });
}
