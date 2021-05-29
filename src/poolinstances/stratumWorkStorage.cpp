#include "poolinstances/stratumWorkStorage.h"

StratumSingleWork::~StratumSingleWork()
{
  for (auto work: LinkedWorks_)
    work->removeLink(this);
}
