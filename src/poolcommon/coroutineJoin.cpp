#include "poolcommon/coroutineJoin.h"
#include "loguru.hpp"
#include <thread>

void coroutineFinishCb(void *arg)
{
  *static_cast<bool*>(arg) = true;
}

void coroutineJoin(const char *threadName, const char *name, bool *finishIndicator)
{
  LOG_F(INFO, "%s: %s finishing", threadName, name);
  while (!*finishIndicator)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
