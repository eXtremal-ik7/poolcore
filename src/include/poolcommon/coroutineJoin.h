#include "asyncio/coroutine.h"

void coroutineFinishCb(void *arg);
void coroutineJoin(const char *threadName, const char *name, bool *finishIndicator);
