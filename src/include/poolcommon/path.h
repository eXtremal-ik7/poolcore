#pragma once

#include <filesystem>
#include <string>

inline std::string path_to_utf8(const std::filesystem::path& p) {
#if __cplusplus >= 202002L
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
#else
    return p.u8string();
#endif
}
