#include "poolcore/dbFormat.h"
#include "loguru.hpp"

bool isDbFormatOutdated(const std::filesystem::path &dbPath, const std::vector<std::string> &coinNames)
{
  // Check root directory
  const std::string rootDbPath[] = {
    "usersettings.2"
  };

  // Check coin directories
  const std::string coinDbPaths[] = {
    "rounds.3",
    "balance.2",
    "foundBlocks.2",
    "poolBalance.2",
    "payouts.2",
    "pplns.payouts.2",
    "workerStats.2",
    "poolstats.2",
    "payoutQueue.raw"
  };

  // Check root directory entries
  if (std::filesystem::exists(dbPath)) {
    std::vector<std::string> existingNames;
    for (std::filesystem::directory_iterator I(dbPath), IE; I != IE; ++I) {
      existingNames.push_back(I->path().filename().generic_string());
    }

    for (const auto &expectedName : rootDbPath) {
      if (std::filesystem::exists(dbPath / expectedName))
        continue;

      auto dotPos = expectedName.find('.');
      std::string baseName = (dotPos != std::string::npos) ? expectedName.substr(0, dotPos) : expectedName;

      for (const auto &fileName : existingNames) {
        if (fileName.starts_with(baseName) && !fileName.ends_with(".bak")) {
          LOG_F(ERROR, "Outdated database format: root has '%s' but expected '%s'", fileName.c_str(), expectedName.c_str());
          return true;
        }
      }
    }
  }

  for (const auto &coinName : coinNames) {
    std::filesystem::path coinDbPath = dbPath / coinName;
    if (!std::filesystem::exists(coinDbPath))
      continue;

    // Collect all entries in coin directory
    std::vector<std::string> existingNames;
    for (std::filesystem::directory_iterator I(coinDbPath), IE; I != IE; ++I) {
      existingNames.push_back(I->path().filename().generic_string());
    }

    for (const auto &expectedName : coinDbPaths) {
      std::filesystem::path currentPath = coinDbPath / expectedName;
      if (std::filesystem::exists(currentPath))
        continue;

      // Extract base name (part before the dot)
      auto dotPos = expectedName.find('.');
      std::string baseName = (dotPos != std::string::npos) ? expectedName.substr(0, dotPos) : expectedName;

      // Check if any similar entry exists
      for (const auto &fileName : existingNames) {
        if (fileName.starts_with(baseName) && !fileName.ends_with(".bak")) {
          LOG_F(ERROR, "Outdated database format: %s has '%s' but expected '%s'", coinName.c_str(), fileName.c_str(), expectedName.c_str());
          return true;
        }
      }
    }
  }

  return false;
}
