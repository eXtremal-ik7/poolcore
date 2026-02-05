#pragma once

#include <deque>
#include <filesystem>
#include "backendData.h"
#include "poolcommon/debug.h"
#include "poolcommon/datFile.h"
#include "poolcommon/file.h"
#include "poolcommon/path.h"
#include "loguru.hpp"
#include "asyncio/asyncio.h"
#include "p2putils/xmstream.h"
#include "poolcore/poolCore.h"
#include <inttypes.h>

struct asyncBase;

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

template<typename CConfig>
class ShareLog {
public:
  ShareLog() {}
  void init(const std::filesystem::path &basePath,
            const std::string &backendName,
            asyncBase *base,
            std::chrono::seconds shareLogFlushInterval,
            int64_t shareLogFileSizeLimit,
            const CConfig &config) {
    Path_ = basePath / "shares.log.3";
    BackendName_ = backendName;
    Base_ = base;
    ShareLogFlushInterval_ = shareLogFlushInterval;
    ShareLogFileSizeLimit_ = shareLogFileSizeLimit;
    Config_ = config;

    enumerateDatFiles(ShareLog_, basePath / "shares.log.3", 3, true);
    if (ShareLog_.empty())
      LOG_F(WARNING, "%s: share log is empty like at first run", BackendName_.c_str());

    Timestamp currentTime = Timestamp::now();
    for (auto &file: ShareLog_)
      replayShares(file);

    Config_.initializationFinish(currentTime);
    CurrentShareId_ = Config_.lastKnownShareId() + 1;

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

  void start() {
    coroutineCall(coroutineNew([](void *arg) { static_cast<ShareLog*>(arg)->shareLogFlushHandler(); }, this, 0x100000));
  }

  void addShare(CShare &share) {
    share.UniqueShareId = CurrentShareId_++;
    // Serialize share to stream
    ShareLogIo<CShare>::serialize(ShareLogInMemory_, share);
  }

  void flush() {
    if (!ShareLoggingEnabled_ || ShareLog_.empty()) {
      ShareLogInMemory_.reset();
      return;
    }

    // Flush memory buffer to disk
    ShareLog_.back().Fd.write(ShareLogInMemory_.data(), ShareLogInMemory_.sizeOf());
    ShareLogInMemory_.reset();

    // Check share log file size limit
    if (ShareLog_.back().Fd.size() >= ShareLogFileSizeLimit_) {
      ShareLog_.back().Fd.close();
      startNewShareLogFile();

      // Check status of shares in previous log files
      uint64_t aggregatedShareId = Config_.lastAggregatedShareId();
      if (isDebugBackend() && !ShareLog_.empty()) {
        LOG_F(1, "Last aggregated share id: %" PRIu64 "; first file range is [%" PRIu64": %" PRIu64 "]", aggregatedShareId, ShareLog_.front().FileId, ShareLog_.front().LastShareId);
      }


      while (ShareLog_.size() > 1 && ShareLog_.front().LastShareId < aggregatedShareId) {
        LOG_F(INFO, "remove old share log file %s", path_to_utf8(ShareLog_.front().Path).c_str());
        std::filesystem::remove(ShareLog_.front().Path);
        ShareLog_.pop_front();
      }
    }
  }

private:
  void replayShares(CDatFile &file) {
    if (isDebugBackend())
      LOG_F(1, "%s: Replaying shares from file %s", BackendName_.c_str(), path_to_utf8(file.Path).c_str());

    FileDescriptor fd;
    if (!fd.open(path_to_utf8(file.Path).c_str())) {
      LOG_F(ERROR, "StatisticDb: can't open file %s", path_to_utf8(file.Path).c_str());
      return;
    }

    size_t fileSize = fd.size();
    xmstream stream(fileSize);
    size_t bytesRead = fd.read(stream.reserve(fileSize), 0, fileSize);
    fd.close();
    if (bytesRead != fileSize) {
      LOG_F(ERROR, "StatisticDb: can't read file %s", path_to_utf8(file.Path).c_str());
      return;
    }

    uint64_t id = 0;
    uint64_t counter = 0;
    uint64_t minShareId = std::numeric_limits<uint64_t>::max();
    uint64_t maxShareId = 0;
    stream.seekSet(0);
    while (stream.remaining()) {
      CShare share;
      ShareLogIo<CShare>::unserialize(stream, share);
      if (stream.eof()) {
        LOG_F(ERROR, "Corrupted file %s", path_to_utf8(file.Path).c_str());
        break;
      }

      if (isDebugBackend()) {
        counter++;
        minShareId = std::min(minShareId, share.UniqueShareId);
        maxShareId = std::max(maxShareId, share.UniqueShareId);
      }

      Config_.replayShare(share);
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
      LOG_F(ERROR, "PoolBackend: can't write to share log %s", path_to_utf8(file.Path).c_str());
      ShareLoggingEnabled_ = false;
    } else {
      LOG_F(INFO, "PoolBackend: started new share log file %s", path_to_utf8(file.Path).c_str());
    }
  }

  void shareLogFlushHandler() {
    aioUserEvent *timerEvent = newUserEvent(Base_, 0, nullptr, nullptr);
    for (;;) {
      ioSleep(timerEvent, std::chrono::microseconds(ShareLogFlushInterval_).count());
      flush();
    }
  }

private:
  std::filesystem::path Path_;
  std::string BackendName_;
  asyncBase *Base_;
  std::chrono::seconds ShareLogFlushInterval_;
  uint64_t ShareLogFileSizeLimit_;
  CConfig Config_;

  xmstream ShareLogInMemory_;
  std::deque<CDatFile> ShareLog_;
  uint64_t CurrentShareId_ = 0;
  bool ShareLoggingEnabled_ = true;
};
