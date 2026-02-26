#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <getopt.h>
#include <filesystem>
#include <string>
#include <thread>

#include "poolcore/plugin.h"
#include "loguru.hpp"

CPluginContext gPluginContext;

// Migration functions
bool migrateV3(const std::filesystem::path &srcPath, const std::filesystem::path &dstPath, unsigned threads, const std::string &statisticCutoff, const std::filesystem::path &priceDbPath = {});
bool fetchPriceHistory(const char *sourceDatabase, const char *dbPath, const char *apiKey);

enum CmdLineOptsTy {
  clOptHelp = 1,
  clOptMode,
  clOptSourceDatabase,
  clOptDestinationDatabase,
  clOptThreads,
  clOptKeepStatistic,
  clOptPriceDb,
  clOptApiKey
};

static option cmdLineOpts[] = {
  {"help", no_argument, nullptr, clOptHelp},
  {"mode", required_argument, nullptr, clOptMode},
  {"source-database", required_argument, nullptr, clOptSourceDatabase},
  {"destination-database", required_argument, nullptr, clOptDestinationDatabase},
  {"threads", required_argument, nullptr, clOptThreads},
  {"keep-statistic", required_argument, nullptr, clOptKeepStatistic},
  {"price-database", required_argument, nullptr, clOptPriceDb},
  {"api-key", required_argument, nullptr, clOptApiKey},
  {nullptr, 0, nullptr, 0}
};

void printHelpMessage()
{
  printf("migrate usage:\n");
  printf("  --mode <name>                   Mode of operation (default: migratev3)\n");
  printf("\n");
  printf("  Mode: migratev3 (database migration v2 -> v3):\n");
  printf("  --source-database <path>        Path to source database directory (required)\n");
  printf("  --destination-database <path>   Path to destination database directory (required, must not exist)\n");
  printf("  --threads <num>                 Number of threads for parallel migration (default: %u)\n", std::max(1u, std::thread::hardware_concurrency() / 2));
  printf("  --keep-statistic <months>       Keep statistic partitions for this many months (default: 36)\n");
  printf("  --price-database <path>          Path to price database (optional, fixes zero exchange rates)\n");
  printf("  --api-key <key>                 CoinGecko API key (required with --price-database)\n");
  printf("\n");
  printf("  Mode: pricefetch (download hourly price history from CoinGecko):\n");
  printf("  --source-database <path>        Path to pool database directory (to discover coins)\n");
  printf("  --price-database <path>         Path to RocksDB database for price data\n");
  printf("  --api-key <key>                 CoinGecko API key\n");
  printf("\n");
  printf("  --help                          Show this help message\n");
}

int main(int argc, char **argv)
{
  loguru::g_stderr_verbosity = loguru::Verbosity_OFF;
  loguru::g_preamble_thread = false;
  loguru::g_preamble_file = true;
  loguru::g_flush_interval_ms = 100;
  loguru::init(argc, argv);
  loguru::g_stderr_verbosity = 1;
  loguru::set_thread_name("main");

  std::string mode = "migratev3";
  const char *sourceDatabase = nullptr;
  const char *destinationDatabase = nullptr;
  const char *priceDbPath = nullptr;
  const char *apiKey = nullptr;
  unsigned threads = std::max(1u, std::thread::hardware_concurrency() / 2);
  unsigned keepStatisticMonths = 36;

  int res;
  int index = 0;
  while ((res = getopt_long(argc, argv, "", cmdLineOpts, &index)) != -1) {
    switch (res) {
      case clOptHelp:
        printHelpMessage();
        return 0;
      case clOptMode:
        mode = optarg;
        break;
      case clOptSourceDatabase:
        sourceDatabase = optarg;
        break;
      case clOptDestinationDatabase:
        destinationDatabase = optarg;
        break;
      case clOptThreads:
        threads = std::max(1, atoi(optarg));
        break;
      case clOptKeepStatistic:
        keepStatisticMonths = std::max(1, atoi(optarg));
        break;
      case clOptPriceDb:
        priceDbPath = optarg;
        break;
      case clOptApiKey:
        apiKey = optarg;
        break;
      case ':':
        fprintf(stderr, "Error: option %s missing argument\n", cmdLineOpts[index].name);
        break;
      case '?':
        exit(1);
      default:
        break;
    }
  }

  if (mode == "pricefetch") {
    if (!sourceDatabase) {
      fprintf(stderr, "Error: --mode=pricefetch requires --source-database <path>\n");
      exit(1);
    }
    if (!priceDbPath) {
      fprintf(stderr, "Error: --mode=pricefetch requires --price-database <path>\n");
      exit(1);
    }
    if (!std::filesystem::exists(sourceDatabase)) {
      fprintf(stderr, "Error: source database path %s does not exist\n", sourceDatabase);
      exit(1);
    }
    if (!apiKey) {
      fprintf(stderr, "Error: --mode=pricefetch requires --api-key <key>\n");
      fprintf(stderr, "Get a free Demo API key at https://www.coingecko.com/en/api/pricing\n");
      exit(1);
    }
    return fetchPriceHistory(sourceDatabase, priceDbPath, apiKey) ? 0 : 1;
  }

  if (mode == "migratev3") {
    if (!sourceDatabase) {
      fprintf(stderr, "Error: you must specify --source-database\n");
      exit(1);
    }

    if (!destinationDatabase) {
      fprintf(stderr, "Error: you must specify --destination-database\n");
      exit(1);
    }

    if (!std::filesystem::exists(sourceDatabase)) {
      fprintf(stderr, "Error: source database path %s does not exist\n", sourceDatabase);
      exit(1);
    }

    if (std::filesystem::exists(destinationDatabase)) {
      fprintf(stderr, "Error: destination database path %s already exists\n", destinationDatabase);
      exit(1);
    }

    std::filesystem::create_directories(destinationDatabase);

    loguru::add_file((std::filesystem::path(destinationDatabase) / "migrate.log").generic_string().c_str(), loguru::Append, loguru::Verbosity_1);

    static loguru::LogChannel masterLog;
    auto masterLogPath = (std::filesystem::path(destinationDatabase) / "logs" / "master" / "log-%Y-%m.log").generic_string();
    masterLog.open(masterLogPath.c_str(), loguru::Append, loguru::Verbosity_1);
    loguru::set_global_channel_log(&masterLog);

    // Compute statistic cutoff partition name (yyyy.mm)
    std::string statisticCutoff;
    {
      time_t now = time(nullptr);
      struct tm tm;
      localtime_r(&now, &tm);
      int year = tm.tm_year + 1900;
      int month = tm.tm_mon + 1;
      month -= static_cast<int>(keepStatisticMonths);
      while (month <= 0) {
        month += 12;
        year--;
      }
      char buf[32];
      snprintf(buf, sizeof(buf), "%04d.%02d", year, month);
      statisticCutoff = buf;
    }

    if (priceDbPath && !apiKey) {
      fprintf(stderr, "Error: --price-database requires --api-key <key>\n");
      exit(1);
    }

    CLOG_F(INFO, "Using {} threads for parallel migration", threads);
    CLOG_F(INFO, "Source: {}", sourceDatabase);
    CLOG_F(INFO, "Destination: {}", destinationDatabase);
    CLOG_F(INFO, "Statistic cutoff: {} (keep {} months)", statisticCutoff, keepStatisticMonths);

    // Update price database before migration
    if (priceDbPath) {
      CLOG_F(INFO, "Updating price database at {}", priceDbPath);
      if (!fetchPriceHistory(sourceDatabase, priceDbPath, apiKey)) {
        CLOG_F(ERROR, "Failed to update price database");
        return 1;
      }
    }

    // Run migrations
    bool success = true;
    success = migrateV3(sourceDatabase, destinationDatabase, threads, statisticCutoff, priceDbPath ? priceDbPath : "") && success;

    if (!success) {
      CLOG_F(ERROR, "Migration failed");
      return 1;
    }

    CLOG_F(INFO, "All migrations completed successfully");
    return 0;
  }

  fprintf(stderr, "Error: unknown mode '%s' (valid modes: migratev3, pricefetch)\n", mode.c_str());
  return 1;
}
