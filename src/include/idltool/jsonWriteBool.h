// Generated JSON write helper — jsonWriteBool
#pragma once

#include <string>

inline void jsonWriteBool(std::string &out, bool v) { out.append(v ? "true" : "false"); }
