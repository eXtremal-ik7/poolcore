#pragma once

#include <deque>
#include <filesystem>
#include <functional>
#include <vector>
#include "poolcommon/debug.h"
#include "poolcommon/datFile.h"
#include "poolcommon/file.h"
#include "poolcommon/path.h"
#include "poolcommon/serialize.h"
#include "loguru.hpp"
#include "p2putils/xmstream.h"
#include <inttypes.h>

template<typename T>
struct ShareLogIo {
  static void serialize(xmstream &out, const T &data);
  static void unserialize(xmstream &in, T &data);
};

template<typename T>
struct ShareLogIo<std::vector<T>> {
  static void serialize(xmstream &out, const std::vector<T> &data) {
    DbIo<uint32_t>::serialize(out, static_cast<uint32_t>(data.size()));
    for (const auto &entry : data)
      DbIo<T>::serialize(out, entry);
  }
  static void unserialize(xmstream &in, std::vector<T> &data) {
    uint32_t count;
    DbIo<uint32_t>::unserialize(in, count);
    data.resize(count);
    for (auto &entry : data)
      DbIo<T>::unserialize(in, entry);
  }
};

template<typename T>
class ShareLog {
public:
  using ReplayShareCallback = std::function<void(uint64_t messageId, const T &data)>;

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

  void startLogging(uint64_t firstMessageId) {
    CurrentMessageId_ = firstMessageId;

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

  uint64_t addShare(const T &data) {
    uint64_t id = CurrentMessageId_++;
    ShareLogInMemory_.writele<uint64_t>(id);
    ShareLogIo<T>::serialize(ShareLogInMemory_, data);
    return id;
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
    uint64_t minMessageId = std::numeric_limits<uint64_t>::max();
    uint64_t maxMessageId = 0;
    stream.seekSet(0);
    while (stream.remaining()) {
      id = stream.readle<uint64_t>();
      T data;
      ShareLogIo<T>::unserialize(stream, data);
      if (stream.eof()) {
        LOG_F(ERROR, "Corrupted file %s", path_to_utf8(file.Path).c_str());
        break;
      }

      if (isDebugBackend()) {
        counter++;
        minMessageId = std::min(minMessageId, id);
        maxMessageId = std::max(maxMessageId, id);
      }

      replayShare(id, data);
    }

    file.LastShareId = id;

    if (isDebugBackend())
      LOG_F(1, "%s: Replayed %" PRIu64 " shares from %" PRIu64 " to %" PRIu64 "", BackendName_.c_str(), counter, minMessageId, maxMessageId);
  }

  void startNewShareLogFile() {
    if (!ShareLog_.empty())
      ShareLog_.back().LastShareId = CurrentMessageId_ - 1;

    auto &file = ShareLog_.emplace_back();
    file.Path = Path_ / (std::to_string(CurrentMessageId_) + ".dat");
    file.FileId = CurrentMessageId_;
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
  uint64_t CurrentMessageId_ = 0;
  bool ShareLoggingEnabled_ = true;
};
