// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "poolcore/thread.h"
#include <asyncio/asyncioTypes.h>
#include <atomic>
 
static std::atomic<unsigned> threadCounter = 0;
// Use 8-byte aligned TLS
static __tls uint64_t globalThreadId;
static __tls uint64_t localThreadId;

void InitializeWorkerThread()
{
  globalThreadId = threadCounter.fetch_add(1);
}

unsigned GetGlobalThreadId()
{
  return static_cast<unsigned>(globalThreadId);
}

unsigned GetLocalThreadId()
{
  return static_cast<unsigned>(localThreadId);
}

void SetLocalThreadId(unsigned threadId)
{
  localThreadId = threadId;
}
