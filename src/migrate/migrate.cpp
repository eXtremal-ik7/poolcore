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
bool migrateV3(const std::filesystem::path &srcPath, const std::filesystem::path &dstPath, unsigned threads, const std::string &statisticCutoff);

enum CmdLineOptsTy {
  clOptHelp = 1,
  clOptSource,
  clOptDestination,
  clOptThreads,
  clOptKeepStatistic
};

static option cmdLineOpts[] = {
  {"help", no_argument, nullptr, clOptHelp},
  {"source", required_argument, nullptr, clOptSource},
  {"destination", required_argument, nullptr, clOptDestination},
  {"threads", required_argument, nullptr, clOptThreads},
  {"keep-statistic", required_argument, nullptr, clOptKeepStatistic},
  {nullptr, 0, nullptr, 0}
};

void printHelpMessage()
{
  printf("migrate usage:\n");
  printf("  --source <path>           Path to source database directory (required)\n");
  printf("  --destination <path>      Path to destination database directory (required, must not exist)\n");
  printf("  --threads <num>           Number of threads for parallel migration (default: %u)\n", std::max(1u, std::thread::hardware_concurrency() / 2));
  printf("  --keep-statistic <months> Keep statistic partitions for this many months (default: 36)\n");
  printf("  --help                    Show this help message\n");
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

  const char *srcPath = nullptr;
  const char *dstPath = nullptr;
  unsigned threads = std::max(1u, std::thread::hardware_concurrency() / 2);
  unsigned keepStatisticMonths = 36;

  int res;
  int index = 0;
  while ((res = getopt_long(argc, argv, "", cmdLineOpts, &index)) != -1) {
    switch (res) {
      case clOptHelp:
        printHelpMessage();
        return 0;
      case clOptSource:
        srcPath = optarg;
        break;
      case clOptDestination:
        dstPath = optarg;
        break;
      case clOptThreads:
        threads = std::max(1, atoi(optarg));
        break;
      case clOptKeepStatistic:
        keepStatisticMonths = std::max(1, atoi(optarg));
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

  if (!srcPath) {
    fprintf(stderr, "Error: you must specify --source\n");
    exit(1);
  }

  if (!dstPath) {
    fprintf(stderr, "Error: you must specify --destination\n");
    exit(1);
  }

  if (!std::filesystem::exists(srcPath)) {
    fprintf(stderr, "Error: source path %s does not exist\n", srcPath);
    exit(1);
  }

  if (std::filesystem::exists(dstPath)) {
    fprintf(stderr, "Error: destination path %s already exists\n", dstPath);
    exit(1);
  }

  std::filesystem::create_directories(dstPath);

  loguru::add_file((std::filesystem::path(dstPath) / "migrate.log").generic_string().c_str(), loguru::Append, loguru::Verbosity_1);

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

  LOG_F(INFO, "Using %u threads for parallel migration", threads);
  LOG_F(INFO, "Source: %s", srcPath);
  LOG_F(INFO, "Destination: %s", dstPath);
  LOG_F(INFO, "Statistic cutoff: %s (keep %u months)", statisticCutoff.c_str(), keepStatisticMonths);

  // Run migrations
  bool success = true;
  success = migrateV3(srcPath, dstPath, threads, statisticCutoff) && success;

  if (!success) {
    LOG_F(ERROR, "Migration failed");
    return 1;
  }

  LOG_F(INFO, "All migrations completed successfully");
  return 0;
}
