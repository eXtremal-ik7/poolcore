#include "poolcore/dbFormat.h"
#include "loguru.hpp"

bool isDbFormatOutdated(const std::filesystem::path &dbPath, const std::vector<std::string> &coinNames)
{
  for (const auto &coinName : coinNames) {
    std::filesystem::path coinDbPath = dbPath / coinName;
    if (!std::filesystem::exists(coinDbPath))
      continue;

    // accounting.state is the definitive marker of current format.
    // Old format uses rounds.v2 + balance + accounting.storage instead.
    if (!std::filesystem::exists(coinDbPath / "accounting.state")) {
      CLOG_F(ERROR, "Outdated database format: {} missing accounting.state — migration required", coinName);
      return true;
    }
  }

  return false;
}
