#pragma once
#include "p2putils/xmstream.h"
#include <stdint.h>

template<unsigned LocalBufferSize>
class SmallStream : public xmstream {
public:
  SmallStream() : xmstream(Buffer_, LocalBufferSize) { reset(); }
private:
  uint8_t Buffer_[LocalBufferSize];
};
