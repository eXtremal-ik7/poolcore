#pragma once

#include <filesystem>
#include <vector>
#include <string>

bool isDbFormatOutdated(const std::filesystem::path &dbPath, const std::vector<std::string> &coinNames);
