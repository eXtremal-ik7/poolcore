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

#if __cplusplus >= 202002L && __cplusplus < 202600L
#include <format>

template<>
struct std::formatter<std::filesystem::path> : std::formatter<std::string> {
  template <class FormatContext>
  auto format(const std::filesystem::path &p, FormatContext &ctx) const {
    return std::formatter<std::string>::format(path_to_utf8(p), ctx);
  }
};
#endif
