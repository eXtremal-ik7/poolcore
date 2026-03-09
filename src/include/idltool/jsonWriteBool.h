// Generated JSON write helper — jsonWriteBool
#pragma once

#include "p2putils/xmstream.h"

inline void jsonWriteBool(xmstream &out, bool v) { out.write(v ? "true" : "false"); }
