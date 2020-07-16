#include "poolcore/clientDispatcher.h"
#include "loguru.hpp"

void CNetworkClientDispatcher::poll()
{
  if (!Clients_.empty()) {
    Clients_[CurrentWorkFetcherIdx]->poll();
  } else {
    LOG_F(ERROR, "%s: no nodes configured", "hz");
  }
}
