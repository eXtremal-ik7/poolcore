#include "blockmaker/stratumWork.h"

StratumSingleWork::~StratumSingleWork()
{
  for (auto work: LinkedWorks_)
    work->removeLink(this);
}
