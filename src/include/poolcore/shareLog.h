#pragma once

#include <deque>
#include <filesystem>
#include <functional>
#include <vector>
#include "poolcommon/crc32.h"
#include "poolcommon/debug.h"
#include "poolcommon/datFile.h"
#include "poolcommon/file.h"
#include "poolcommon/path.h"
#include "poolcommon/serialize.h"
#include "loguru.hpp"
#include "p2putils/xmstream.h"
#include <inttypes.h>

struct ShareLogMsgHeader {
  uint64_t Id;
  uint32_t Length;
  uint32_t Crc32;
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

  // Record format: [ShareLogMsgHeader][data]
  // CRC32C covers data bytes (Length bytes after header)
  uint64_t addMessage(const T &data) {
    uint64_t id = CurrentMessageId_++;

    // Reserve header, serialize data
    size_t headerPos = ShareLogInMemory_.offsetOf();
    ShareLogInMemory_.reserve(sizeof(ShareLogMsgHeader));
    size_t dataStart = ShareLogInMemory_.offsetOf();
    DbIo<T>::serialize(ShareLogInMemory_, data);
    size_t dataEnd = ShareLogInMemory_.offsetOf();

    uint32_t dataSize = static_cast<uint32_t>(dataEnd - dataStart);

    // Patch header
    ShareLogMsgHeader *header = reinterpret_cast<ShareLogMsgHeader*>(
      static_cast<uint8_t*>(ShareLogInMemory_.data()) + headerPos
    );
    header->Id = xhtole(id);
    header->Length = xhtole(dataSize);
    header->Crc32 = xhtole(crc32c(
      static_cast<const uint8_t*>(ShareLogInMemory_.data()) + dataStart,
      dataSize
    ));

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
      if (stream.remaining() < sizeof(ShareLogMsgHeader)) {
        LOG_F(ERROR, "ShareLog: truncated header in %s", path_to_utf8(file.Path).c_str());
        break;
      }

      const ShareLogMsgHeader *header =
        reinterpret_cast<const ShareLogMsgHeader*>(stream.seek<uint8_t>(sizeof(ShareLogMsgHeader)));
      id = xletoh(header->Id);
      uint32_t dataSize = xletoh(header->Length);
      uint32_t storedCrc = xletoh(header->Crc32);

      if (dataSize > stream.remaining()) {
        LOG_F(ERROR, "ShareLog: truncated data in %s", path_to_utf8(file.Path).c_str());
        break;
      }

      uint8_t *dataPtr = stream.seek<uint8_t>(dataSize);
      uint32_t computedCrc = crc32c(dataPtr, dataSize);
      if (computedCrc != storedCrc) {
        LOG_F(ERROR,
          "ShareLog: CRC32C mismatch in %s (stored %08X, computed %08X)",
          path_to_utf8(file.Path).c_str(),
          storedCrc,
          computedCrc);
        break;
      }

      xmstream payload(dataPtr, dataSize);
      T data;
      DbIo<T>::unserialize(payload, data);
      if (payload.eof()) {
        LOG_F(ERROR, "ShareLog: corrupted payload in %s", path_to_utf8(file.Path).c_str());
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
