// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <filesystem>
#ifdef WIN32
typedef void* HANDLE;
#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif
#endif

class FileDescriptor {
public:
  FileDescriptor();
  bool open(const std::filesystem::path &path);
  void close();
  size_t size();
  size_t seekSet(size_t position);
  ssize_t read(void *data, size_t offset, size_t size);
  ssize_t write(const void *data, size_t size);
  ssize_t write(const void *data, size_t offset, size_t size);
  bool truncate(size_t size);

  bool isOpened();
  int fd();
private:
#ifndef _WIN32
  int Fd_;
#else
  static thread_local HANDLE Event_;
  HANDLE Fd_;
  HANDLE event();
#endif
};

