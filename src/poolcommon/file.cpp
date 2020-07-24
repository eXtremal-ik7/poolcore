// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "poolcommon/file.h"

#ifndef _WIN32 // POSIX implementation
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

FileDescriptor::FileDescriptor()
{
  Fd_ = -1;
}

bool FileDescriptor::open(const std::filesystem::path &path)
{
  if (Fd_ > 0)
    ::close(Fd_);
  Fd_ = ::open(path.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  return Fd_ > 0;
}

void FileDescriptor::close()
{
  ::close(Fd_);
  Fd_ = -1;
}

size_t FileDescriptor::size()
{
  struct stat st;
  fstat(Fd_, &st);
  return st.st_size;
}

size_t FileDescriptor::seekSet(size_t position)
{
  return lseek64(Fd_, position, SEEK_SET);
}

ssize_t FileDescriptor::read(void *data, size_t offset, size_t size)
{
  uint8_t *ptr = static_cast<uint8_t*>(data);
  size_t remaining = size;
  ssize_t pread64ReturnValue;
  while ( (pread64ReturnValue = pread(Fd_, ptr, remaining, offset)) > 0) {
    remaining -= pread64ReturnValue;
    ptr += pread64ReturnValue;
    offset += pread64ReturnValue;
    if (remaining == 0)
      return ptr - static_cast<uint8_t*>(data);
  }

  return pread64ReturnValue == 0 ? ptr - static_cast<uint8_t*>(data) : -1;
}

ssize_t FileDescriptor::write(const void *data, size_t size)
{
  const uint8_t *ptr = static_cast<const uint8_t*>(data);
  size_t remaining = size;
  ssize_t pread64ReturnValue;
  while ( (pread64ReturnValue = ::write(Fd_, ptr, remaining)) > 0) {
    remaining -= pread64ReturnValue;
    ptr += pread64ReturnValue;
    if (remaining == 0) {
      fsync(Fd_);
      return ptr - static_cast<const uint8_t*>(data);
    }
  }

  fsync(Fd_);
  return pread64ReturnValue == 0 ? ptr - static_cast<const uint8_t*>(data) : -1;
}

ssize_t FileDescriptor::write(const void *data, size_t offset, size_t size)
{
  const uint8_t *ptr = static_cast<const uint8_t*>(data);
  size_t remaining = size;
  ssize_t pread64ReturnValue;
  while ( (pread64ReturnValue = pwrite(Fd_, ptr, remaining, offset)) > 0) {
    remaining -= pread64ReturnValue;
    ptr += pread64ReturnValue;
    offset += pread64ReturnValue;
    if (remaining == 0) {
      fsync(Fd_);
      return ptr - static_cast<const uint8_t*>(data);
    }
  }

  fsync(Fd_);
  return pread64ReturnValue == 0 ? ptr - static_cast<const uint8_t*>(data) : -1;
}

bool FileDescriptor::truncate(size_t size)
{
  return ftruncate64(Fd_, size) == 0;
}

bool FileDescriptor::isOpened()
{
  return Fd_ > 0;
}

int FileDescriptor::fd()
{
  return Fd_;
}

#else // Win32 implementation

#include <Windows.h>

thread_local HANDLE FileDescriptor::Event_ = INVALID_HANDLE_VALUE;

HANDLE FileDescriptor::event()
{
  if (Event_ == INVALID_HANDLE_VALUE)
    Event_ = CreateEvent(NULL, FALSE, FALSE, NULL);
  return Event_;
}

FileDescriptor::FileDescriptor()
{
  Fd_ = INVALID_HANDLE_VALUE;
}

bool FileDescriptor::open(const std::filesystem::path& path)
{
  Fd_ = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
  return isOpened();
}

void FileDescriptor::close()
{
  CloseHandle(Fd_);
}

uint64_t FileDescriptor::size()
{
  DWORD fileSizeHigh = 0;
  DWORD fileSizeLow = GetFileSize(Fd_, &fileSizeHigh);
  return (static_cast<uint64_t>(fileSizeHigh) << 32) | fileSizeLow;
}

uint64_t FileDescriptor::seekSet(uint64_t position)
{
  LONG hiWord = position >> 32;
  DWORD result = SetFilePointer(Fd_, static_cast<LONG>(position & 0xFFFFFFFF), &hiWord, FILE_BEGIN);
  return (static_cast<uint64_t>(hiWord) << 32) | result;
}

ssize_t FileDescriptor::read(void *data, size_t offset, size_t size)
{
  size_t remaining = size;
  while (remaining) {
    BOOL result;
    size_t currentReadSize = std::min(remaining, static_cast<size_t>(std::numeric_limits<DWORD>::max()));

    OVERLAPPED ov = { 0 };
    ov.hEvent = event();
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    result = ReadFile(Fd_, data, static_cast<DWORD>(currentReadSize), NULL, &ov);
    if (result == FALSE && GetLastError() != ERROR_IO_PENDING)
      return -1;

    DWORD bytesTransferred = 0;
    result = GetOverlappedResult(Fd_, &ov, &bytesTransferred, TRUE);

    remaining -= bytesTransferred;
    offset += bytesTransferred;

    if (result == FALSE)
      return -1;
    if (bytesTransferred != currentReadSize)
      break;
  }

  return size - remaining;
}

ssize_t FileDescriptor::write(const void *data, size_t size)
{
  size_t remaining = size;
  while (remaining) {
    BOOL result;
    size_t currentReadSize = std::min(remaining, static_cast<size_t>(std::numeric_limits<DWORD>::max()));
    DWORD bytesTransferred = 0;
    result = WriteFile(Fd_, data, static_cast<DWORD>(currentReadSize), &bytesTransferred, NULL);
    if (result == FALSE)
      return -1;

    remaining -= bytesTransferred;
    if (bytesTransferred != currentReadSize)
      break;
  }

  return size - remaining;
}

ssize_t FileDescriptor::write(const void *data, size_t offset, size_t size)
{
  size_t remaining = size;
  while (remaining) {
    BOOL result;
    size_t currentReadSize = std::min(remaining, static_cast<size_t>(std::numeric_limits<DWORD>::max()));

    OVERLAPPED ov = { 0 };
    ov.hEvent = event();
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    result = WriteFile(Fd_, data, static_cast<DWORD>(currentReadSize), NULL, &ov);
    if (result == FALSE && GetLastError() != ERROR_IO_PENDING)
      return -1;

    DWORD bytesTransferred = 0;
    result = GetOverlappedResult(Fd_, &ov, &bytesTransferred, TRUE);

    remaining -= bytesTransferred;
    offset += bytesTransferred;

    if (result == FALSE)
      return -1;
    if (bytesTransferred != currentReadSize)
      break;
  }

  return size - remaining;
}

bool FileDescriptor::truncate(size_t size)
{
  LONG hiWord = size >> 32;
  SetFilePointer(Fd_, static_cast<LONG>(size), &hiWord, FILE_BEGIN);
  return SetEndOfFile(Fd_);
}

bool FileDescriptor::isOpened()
{
  return Fd_ != INVALID_HANDLE_VALUE;
}

int FileDescriptor::fd()
{
  return reinterpret_cast<int>(Fd_);
}
#endif
