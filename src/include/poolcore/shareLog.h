#pragma once

#include <concepts>
#include <deque>
#include <filesystem>
#include <functional>
#include "backendData.h"
#include "poolcommon/debug.h"
#include "poolcommon/datFile.h"
#include "poolcommon/file.h"
#include "poolcommon/path.h"
#include "loguru.hpp"
#include "p2putils/xmstream.h"
#include <inttypes.h>

template<typename T>
struct ShareLogIo {
  static void serialize(xmstream &out, const T &data);
  static void unserialize(xmstream &in, T &data);
};

template<>
struct ShareLogIo<CShare> {
  static void serialize(xmstream &out, const CShare &data);
  static void unserialize(xmstream &in, CShare &data);
};

template<typename T>
concept ShareLogEntry = std::default_initializable<T> && requires(T entry, xmstream &stream, uint64_t id) {
  { entry.UniqueShareId = id };
  { entry.UniqueShareId } -> std::convertible_to<uint64_t>;
  ShareLogIo<T>::serialize(stream, entry);
  ShareLogIo<T>::unserialize(stream, entry);
};

template<ShareLogEntry T>
class ShareLog {
public:
  using ReplayShareCallback = std::function<void(const T&)>;

  ShareLog(const std::filesystem::path &logPath,
           const std::string &backendName,
           int64_t shareLogFileSizeLimit) :
    Path_(logPath), BackendName_(backendName), ShareLogFileSizeLimit_(shareLogFileSizeLimit) {}

  void replay(ReplayShareCallback replayShare) {
    enumerateDatFiles(ShareLog_, Path_, 3, true);
    if (ShareLog_.empty())
      LOG_F(WARNING, "%s: share log is empty like at first run", BackendName_.c_str());

    for (auto &file: ShareLog_)
      replayShares(file, replayShare);
  }

  void startLogging(uint64_t firstShareId) {
    CurrentShareId_ = firstShareId;

    if (!ShareLog_.empty()) {
      CDatFile &lastFile = ShareLog_.back();
      if (lastFile.Fd.open(lastFile.Path)) {
        lastFile.Fd.seekSet(lastFile.Fd.size());
      } else {
        LOG_F(ERROR, "Can't open share log %s", path_to_utf8(lastFile.Path).c_str());
        ShareLoggingEnabled_ = false;
      }
    } else {
      startNewShareLogFile();
    }
  }

  void addShare(T &share) {
    share.UniqueShareId = CurrentShareId_++;
    ShareLogIo<T>::serialize(ShareLogInMemory_, share);
  }

  void flush() {
    if (!ShareLoggingEnabled_ || ShareLog_.empty()) {
      ShareLogInMemory_.reset();
      return;
    }

    ShareLog_.back().Fd.write(ShareLogInMemory_.data(), ShareLogInMemory_.sizeOf());
    ShareLogInMemory_.reset();

    if (ShareLog_.back().Fd.size() >= ShareLogFileSizeLimit_) {
      ShareLog_.back().Fd.close();
      startNewShareLogFile();
    }
  }

  void cleanupOldFiles(uint64_t lastAggregatedShareId) {
    if (isDebugBackend() && !ShareLog_.empty()) {
      LOG_F(1, "Last aggregated share id: %" PRIu64 "; first file range is [%" PRIu64 ": %" PRIu64 "]", lastAggregatedShareId, ShareLog_.front().FileId, ShareLog_.front().LastShareId);
    }

    while (ShareLog_.size() > 1 && ShareLog_.front().LastShareId < lastAggregatedShareId) {
      LOG_F(INFO, "remove old share log file %s", path_to_utf8(ShareLog_.front().Path).c_str());
      std::filesystem::remove(ShareLog_.front().Path);
      ShareLog_.pop_front();
    }
  }

private:
  void replayShares(CDatFile &file, const ReplayShareCallback &replayShare) {
    if (isDebugBackend())
      LOG_F(1, "%s: Replaying shares from file %s", BackendName_.c_str(), path_to_utf8(file.Path).c_str());

    FileDescriptor fd;
    if (!fd.open(path_to_utf8(file.Path).c_str())) {
      LOG_F(ERROR, "ShareLog: can't open file %s", path_to_utf8(file.Path).c_str());
      return;
    }

    size_t fileSize = fd.size();
    xmstream stream(fileSize);
    size_t bytesRead = fd.read(stream.reserve(fileSize), 0, fileSize);
    fd.close();
    if (bytesRead != fileSize) {
      LOG_F(ERROR, "ShareLog: can't read file %s", path_to_utf8(file.Path).c_str());
      return;
    }

    uint64_t id = 0;
    uint64_t counter = 0;
    uint64_t minShareId = std::numeric_limits<uint64_t>::max();
    uint64_t maxShareId = 0;
    stream.seekSet(0);
    while (stream.remaining()) {
      T share;
      ShareLogIo<T>::unserialize(stream, share);
      if (stream.eof()) {
        LOG_F(ERROR, "Corrupted file %s", path_to_utf8(file.Path).c_str());
        break;
      }

      if (isDebugBackend()) {
        counter++;
        minShareId = std::min(minShareId, share.UniqueShareId);
        maxShareId = std::max(maxShareId, share.UniqueShareId);
      }

      replayShare(share);
      id = share.UniqueShareId;
    }

    file.LastShareId = id;

    if (isDebugBackend())
      LOG_F(1, "%s: Replayed %" PRIu64 " shares from %" PRIu64 " to %" PRIu64 "", BackendName_.c_str(), counter, minShareId, maxShareId);
  }

  void startNewShareLogFile() {
    if (!ShareLog_.empty())
      ShareLog_.back().LastShareId = CurrentShareId_ - 1;

    auto &file = ShareLog_.emplace_back();
    file.Path = Path_ / (std::to_string(CurrentShareId_) + ".dat");
    file.FileId = CurrentShareId_;
    file.LastShareId = 0;
    if (!file.Fd.open(file.Path)) {
      LOG_F(ERROR, "ShareLog: can't write to share log %s", path_to_utf8(file.Path).c_str());
      ShareLoggingEnabled_ = false;
    } else {
      LOG_F(INFO, "ShareLog: started new share log file %s", path_to_utf8(file.Path).c_str());
    }
  }

private:
  std::filesystem::path Path_;
  std::string BackendName_;
  uint64_t ShareLogFileSizeLimit_ = 0;

  xmstream ShareLogInMemory_;
  std::deque<CDatFile> ShareLog_;
  uint64_t CurrentShareId_ = 0;
  bool ShareLoggingEnabled_ = true;
};
