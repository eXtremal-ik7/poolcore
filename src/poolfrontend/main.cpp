#include "config.h"
#include "http.h"

#include "poolcore/bitcoinRPCClient.h"
#include "poolcore/ethereumRPCClient.h"

#include "poolcore/backend.h"
#include "poolcore/coinLibrary.h"
#include "poolcore/dbFormat.h"
#include "poolcore/clientDispatcher.h"
#include "poolcore/plugin.h"
#include "poolcore/thread.h"
#include "poolcommon/utils.h"
#include "poolinstances/fabric.h"

#include "asyncio/asyncio.h"
#include "asyncio/socket.h"
#include "loguru.hpp"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <thread>
#if !defined(OS_WINDOWS)
#include <netdb.h>
#endif

#if defined(OS_LINUX)
extern "C" int mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
#endif

static int interrupted = 0;
static int sigusrReceived = 0;
static void sigIntHandler(int) { interrupted = 1; }
static void sigUsrHandler(int) { sigusrReceived = 1; }

static void processSigUsr()
{
#if defined(OS_LINUX)
  mallctl("prof.dump", NULL, NULL, NULL, 0);
#endif
}

struct PoolContext {
  bool IsMaster;
  std::filesystem::path DatabasePath;
  uint16_t HttpPort;

  std::vector<CCoinInfo> CoinList;
  std::unique_ptr<CPriceFetcher> PriceFetcher;
  std::vector<std::unique_ptr<CNetworkClientDispatcher>> ClientDispatchers;
  std::vector<std::unique_ptr<PoolBackend>> Backends;
  std::vector<std::unique_ptr<StatisticServer>> AlgoMetaStatistic;

  std::unique_ptr<CThreadPool> ThreadPool;
  std::vector<std::unique_ptr<CPoolInstance>> Instances;

  std::unique_ptr<UserManager> UserMgr;
  std::unique_ptr<PoolHttpServer> HttpServer;
  std::unique_ptr<ComplexMiningStats> MiningStats;
};

CPluginContext gPluginContext;

std::filesystem::path userHomeDir()
{
  char homedir[512];
#ifdef _WIN32
  snprintf(homedir, sizeof(homedir), "%s%s", getenv("HOMEDRIVE"), getenv("HOMEPATH"));
#else
  snprintf(homedir, sizeof(homedir), "%s", getenv("HOME"));
#endif
  return homedir;
}

int main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <configuration file>\n", argv[0]);
    return 1;
  }

  loguru::g_preamble_thread = true;
  loguru::g_preamble_file = true;
  loguru::g_flush_interval_ms = 100;
  loguru::g_stderr_verbosity = loguru::Verbosity_1;
  loguru::init(argc, argv);
  loguru::set_thread_name("main");

  PoolBackendConfig backendConfig;
  PoolContext poolContext;
  unsigned totalThreadsNum = 0;
  unsigned workerThreadsNum = 0;
  unsigned httpThreadsNum = 0;

  initializeSocketSubsystem();
  asyncBase *monitorBase = createAsyncBase(amOSDefault);

  // Parse config
  FileDescriptor configFd;
  if (!configFd.open(argv[1])) {
    LOG_F(ERROR, "Can't open config file %s", argv[1]);
    return 1;
  }

  std::string configData;
  configData.resize(configFd.size());
  configFd.read(configData.data(), 0, configData.size());
  configFd.close();
  rapidjson::Document document;
  document.Parse<rapidjson::kParseCommentsFlag>(configData.c_str());
  if (document.HasParseError()) {
    LOG_F(ERROR, "Config file %s is not valid JSON", argv[1]);
    return 1;
  }

  CPoolFrontendConfig config;
  {
    std::string error;
    if (!config.load(document, error)) {
      LOG_F(ERROR, "Config file %s contains error", argv[1]);
      LOG_F(ERROR, "%s", error.c_str());
      return 1;
    }
  }

  {
    if (config.DbPath.starts_with("~/") || config.DbPath.starts_with("~\\"))
      poolContext.DatabasePath = userHomeDir() / (config.DbPath.data()+2);
    else
      poolContext.DatabasePath = config.DbPath;

    {
      char logFileName[64];
      auto t = time(nullptr);
      auto now = localtime(&t);
      snprintf(logFileName, sizeof(logFileName), "poolfrontend-%04u-%02u-%02u.log", now->tm_year + 1900, now->tm_mon + 1, now->tm_mday);
      std::filesystem::path logFilePath(poolContext.DatabasePath);
      loguru::add_file((poolContext.DatabasePath / logFileName).generic_string().c_str(), loguru::Append, loguru::Verbosity_1);
    }

    // Check for outdated database format
    {
      std::vector<std::string> coinNames;
      for (const auto &coin : config.Coins)
        coinNames.push_back(coin.Name);
      if (isDbFormatOutdated(poolContext.DatabasePath, coinNames)) {
        LOG_F(ERROR, "database needs to be manually updated with migrate tool");
        return 1;
      }
    }

    // Analyze config
    poolContext.IsMaster = config.IsMaster;
    poolContext.HttpPort = config.HttpPort;
    workerThreadsNum = config.WorkerThreadsNum;
    httpThreadsNum = config.HttpThreadsNum;
    if (workerThreadsNum == 0)
      workerThreadsNum = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() / 4 : 2;
    if (httpThreadsNum == 0)
      httpThreadsNum = 1;

    // Calculate total threads num
    unsigned backendsNum = static_cast<unsigned>(config.Coins.size());
    totalThreadsNum =
      1 +                   // Monitor (listeners and clients polling)
      workerThreadsNum +    // Share checkers
      backendsNum*2 +       // Backends & metastatistic algorithm servers
      httpThreadsNum +      // HTTP server
      1;                    // Complex mining stats service
    LOG_F(INFO, "Worker threads: %u; total pool threads: %u", workerThreadsNum, totalThreadsNum);

    // Initialize user manager
    poolContext.UserMgr.reset(new UserManager(poolContext.DatabasePath));

    // Base config
    poolContext.UserMgr->setBaseCfg(config.PoolName,
                                    config.PoolHostProtocol,
                                    config.PoolHostAddress,
                                    config.PoolActivateLinkPrefix,
                                    config.PoolChangePasswordLinkPrefix,
                                    config.PoolActivate2faLinkPrefix,
                                    config.PoolDeactivate2faLinkPrefix);

    // Admin & observer passwords
    if (!config.AdminPasswordHash.empty())
      poolContext.UserMgr->addSpecialUser(UserManager::ESpecialUserAdmin, config.AdminPasswordHash);
    if (!config.ObserverPasswordHash.empty())
      poolContext.UserMgr->addSpecialUser(UserManager::ESpecialUserObserver, config.ObserverPasswordHash);

    // SMTP config
    if (config.SmtpEnabled) {
      // Build HostAddress for server
      HostAddress smtpAddress;
      char *colonPos = (char*)strchr(config.SmtpServer.c_str(), ':');
      if (colonPos == nullptr) {
        LOG_F(ERROR, "Invalid server %s\nIt must have address:port format", config.SmtpServer.c_str());
        return 1;
      }

      *colonPos = 0;
      hostent *host = gethostbyname(config.SmtpServer.c_str());
      if (!host) {
        LOG_F(ERROR, "Cannot retrieve address of %s (gethostbyname failed)", config.SmtpServer.c_str());
      }

      u_long addr = host->h_addr ? *reinterpret_cast<u_long*>(host->h_addr) : 0;
      if (!addr) {
        LOG_F(ERROR, "Cannot retrieve address of %s (gethostbyname returns 0)", config.SmtpServer.c_str());
        return 1;
      }

      smtpAddress.family = AF_INET;
      smtpAddress.ipv4 = static_cast<uint32_t>(addr);
      smtpAddress.port = htons(atoi(colonPos + 1));

      // Enable SMTP
      poolContext.UserMgr->enableSMTP(smtpAddress, config.SmtpLogin, config.SmtpPassword, config.SmtpSenderAddress, config.SmtpUseSmtps, config.SmtpUseStartTls);
    }

    // Lookup information for all coins
    for (const auto &coin: config.Coins) {
      const char *coinName = coin.Name.c_str();
      CCoinInfo coinInfo = CCoinLibrary::get(coinName);
      if (coinInfo.Name.empty()) {
        // load coin info from extra directory
        bool foundInExtra = false;
        for (const auto &proc: gPluginContext.AddExtraCoinProcs) {
          if (proc(coinName, coinInfo)) {
            foundInExtra = true;
            break;
          }
        }

        if (!foundInExtra) {
          LOG_F(ERROR, "Unknown coin: %s", coinName);
          return 1;
        }
      }

      poolContext.CoinList.push_back(coinInfo);
    }

    // Initialize price fetcher
    poolContext.PriceFetcher.reset(new CPriceFetcher(monitorBase, poolContext.CoinList));

    // Initialize all backends
    std::map<std::string, StatisticServer*> knownAlgo;
    for (size_t coinIdx = 0, coinIdxE = config.Coins.size(); coinIdx != coinIdxE; ++coinIdx) {
      PoolBackendConfig backendConfig;
      const CCoinConfig &coinConfig = config.Coins[coinIdx];
      const char *coinName = coinConfig.Name.c_str();
      CCoinInfo &coinInfo = poolContext.CoinList[coinIdx];

      // Inherited pool config parameters
      backendConfig.isMaster = poolContext.IsMaster;
      backendConfig.dbPath = poolContext.DatabasePath / coinInfo.Name;

      // Backend parameters
      if (!parseMoneyValue(coinConfig.DefaultPayoutThreshold.c_str(), coinInfo.FractionalPartSize, &backendConfig.DefaultPayoutThreshold)) {
        LOG_F(ERROR, "Can't load 'defaultPayoutThreshold' from %s coin config", coinName);
        return 1;
      }

      if (!parseMoneyValue(coinConfig.MinimalAllowedPayout.c_str(), coinInfo.FractionalPartSize, &backendConfig.MinimalAllowedPayout)) {
        LOG_F(ERROR, "Can't load 'minimalPayout' from %s coin config", coinName);
        return 1;
      }

      if (coinConfig.RequiredConfirmations < coinInfo.MinimalConfirmationsNumber) {
        LOG_F(ERROR, "Minimal required confirmations for %s is %u", coinInfo.Name.c_str(), coinInfo.MinimalConfirmationsNumber);
        return 1;
      }

      backendConfig.RequiredConfirmations = coinConfig.RequiredConfirmations;
      backendConfig.KeepRoundTime = coinConfig.KeepRoundTime * 24*3600;
      backendConfig.KeepStatsTime = coinConfig.KeepStatsTime * 60;
      backendConfig.ConfirmationsCheckInterval = coinConfig.ConfirmationsCheckInterval * 60 * 1000000;
      backendConfig.PayoutInterval = coinConfig.PayoutInterval * 60 * 1000000;
      backendConfig.BalanceCheckInterval = coinConfig.BalanceCheckInterval * 60 * 1000000;

      for (const auto &addr: coinConfig.MiningAddresses) {
        if (!coinInfo.checkAddress(addr.Address, coinInfo.PayoutAddressType)) {
          LOG_F(ERROR, "Invalid mining address: %s", addr.Address.c_str());
          return 1;
        }

        backendConfig.MiningAddresses.add(CMiningAddress(addr.Address, addr.PrivateKey), addr.Weight);
      }

      backendConfig.PPSPayoutInterval = std::chrono::minutes(coinConfig.PPSPayoutInterval);
      backendConfig.CoinBaseMsg = coinConfig.CoinbaseMsg;
      if (backendConfig.CoinBaseMsg.empty())
        backendConfig.CoinBaseMsg = config.PoolName;

      // ZEC specific
      backendConfig.poolZAddr = coinConfig.PoolZAddr;
      backendConfig.poolTAddr = coinConfig.PoolTAddr;

      // Nodes
      std::unique_ptr<CNetworkClientDispatcher> dispatcher(new CNetworkClientDispatcher(monitorBase, coinInfo, totalThreadsNum));
      for (size_t nodeIdx = 0, nodeIdxE = coinConfig.GetWorkNodes.size(); nodeIdx != nodeIdxE; ++nodeIdx) {
        CNetworkClient *client;
        const CNodeConfig &node = coinConfig.GetWorkNodes[nodeIdx];
        if (node.Type == "bitcoinrpc") {
          client = new CBitcoinRpcClient(monitorBase, totalThreadsNum, coinInfo, node.Address.c_str(), node.Login.c_str(), node.Password.c_str(), node.Wallet.c_str(), node.LongPollEnabled);
        } else if (node.Type == "ethereumrpc") {
          client = new CEthereumRpcClient(monitorBase, totalThreadsNum, coinInfo, node.Address.c_str(), backendConfig);
        } else {
          // lookup client type in extras
          for (const auto &proc: gPluginContext.AddRpcClientProcs) {
            client = proc(node.Type, monitorBase, totalThreadsNum, coinInfo, node, backendConfig);
            if (client)
              break;
          }

          if (!client) {
            LOG_F(ERROR, "Unknown node type: %s", node.Type.c_str());
            return 1;
          }
        }

        dispatcher->addGetWorkClient(client);
      }

      for (size_t nodeIdx = 0, nodeIdxE = coinConfig.RPCNodes.size(); nodeIdx != nodeIdxE; ++nodeIdx) {
        CNetworkClient *client;
        const CNodeConfig &node = coinConfig.RPCNodes[nodeIdx];
        if (node.Type == "bitcoinrpc") {
          client = new CBitcoinRpcClient(monitorBase, totalThreadsNum, coinInfo, node.Address.c_str(), node.Login.c_str(), node.Password.c_str(), node.Wallet.c_str(), node.LongPollEnabled);
        } else if (node.Type == "ethereumrpc") {
          client = new CEthereumRpcClient(monitorBase, totalThreadsNum, coinInfo, node.Address.c_str(), backendConfig);
        } else {
          // lookup client type in extras
          for (const auto &proc: gPluginContext.AddRpcClientProcs) {
            client = proc(node.Type, monitorBase, totalThreadsNum, coinInfo, node, backendConfig);
            if (client)
              break;
          }

          if (!client) {
            LOG_F(ERROR, "Unknown node type: %s", node.Type.c_str());
            return 1;
          }
        }

        dispatcher->addRPCClient(client);
      }

      // Initialize backend
      PoolBackend *backend = new PoolBackend(createAsyncBase(amOSDefault), std::move(backendConfig), coinInfo, *poolContext.UserMgr, *dispatcher, *poolContext.PriceFetcher);

      poolContext.Backends.emplace_back(backend);
      if (coinConfig.ProfitSwitchCoeff != 0.0)
        backend->setProfitSwitchCoeff(coinConfig.ProfitSwitchCoeff);
      poolContext.ClientDispatchers.emplace_back(dispatcher.release());
      poolContext.UserMgr->configAddCoin(coinInfo, backendConfig.DefaultPayoutThreshold, backend);

      // Initialize algorithm meta statistic
      auto AlgoIt = knownAlgo.find(coinInfo.Algorithm);
      if (AlgoIt == knownAlgo.end()) {
        CCoinInfo algoInfo = CCoinLibrary::get(coinInfo.Algorithm.c_str());
        if (algoInfo.Name.empty()) {
          LOG_F(ERROR, "Coin %s has unknown algorithm: %s", coinInfo.Name.c_str(), coinInfo.Algorithm.c_str());
          return 1;
        }

        PoolBackendConfig algoConfig;
        algoConfig.dbPath = poolContext.DatabasePath / algoInfo.Name;
        StatisticServer *server = new StatisticServer(createAsyncBase(amOSDefault), algoConfig, algoInfo);
        poolContext.AlgoMetaStatistic.emplace_back(server);
        AlgoIt = knownAlgo.insert(AlgoIt, std::make_pair(coinInfo.Algorithm, server));
      }

      backend->setAlgoMetaStatistic(AlgoIt->second);
    }

    std::sort(poolContext.Backends.begin(), poolContext.Backends.end(), [](const auto &l, const auto &r) { return l->getCoinInfo().Name < r->getCoinInfo().Name; });

    // Initialize "complex mining stats" service
    poolContext.MiningStats.reset(!gPluginContext.HasMiningStatsHandler ?
      new ComplexMiningStats :
      gPluginContext.CreateMiningStatsHandlerProc(poolContext.Backends, poolContext.DatabasePath));

    // Initialize workers
    poolContext.ThreadPool.reset(new CThreadPool(workerThreadsNum));

    // Initialize instances
    poolContext.Instances.resize(config.Instances.size());
    for (size_t instIdx = 0, instIdxE = config.Instances.size(); instIdx != instIdxE; ++instIdx) {
      CInstanceConfig &instanceConfig = config.Instances[instIdx];
      std::vector<PoolBackend*> linkedBackends;

      // Get linked backends
      for (const auto &linkedCoinName: instanceConfig.Backends) {
        auto It = std::lower_bound(poolContext.Backends.begin(), poolContext.Backends.end(), linkedCoinName, [](const auto &backend, const std::string &name) { return backend->getCoinInfo().Name < name; });
        if (It == poolContext.Backends.end() || (*It)->getCoinInfo().Name != linkedCoinName) {
          LOG_F(ERROR, "Instance %s linked with non-existent coin %s", instanceConfig.Name.c_str(), linkedCoinName.c_str());
          return 1;
        }

        linkedBackends.push_back(It->get());
      }


      // Validate that all linked backends use the same algorithm
      std::string algo;
      for (PoolBackend *linkedBackend: linkedBackends) {
        if (!algo.empty() && linkedBackend->getCoinInfo().Algorithm != algo) {
          LOG_F(ERROR,
                "Linked backends with different algorithms (%s and %s) to one instance %s",
                algo.c_str(),
                linkedBackend->getCoinInfo().Algorithm.c_str(),
                instanceConfig.Name.c_str());
          return 1;
        }

        algo = linkedBackend->getCoinInfo().Algorithm;
      }

      StatisticServer *algoMetaStatistic = linkedBackends[0]->getAlgoMetaStatistic();
      CPoolInstance *instance = PoolInstanceFabric::get(
        monitorBase,
        *poolContext.UserMgr,
        linkedBackends,
        *poolContext.ThreadPool,
        algoMetaStatistic,
        poolContext.MiningStats.get(),
        instanceConfig.Type,
        instanceConfig.Protocol,
        static_cast<unsigned>(instIdx),
        static_cast<unsigned>(instIdxE),
        instanceConfig.InstanceConfig,
        poolContext.PriceFetcher.get());
      if (!instance) {
        // Try create pool instances using extras
        for (const auto &proc: gPluginContext.CreatePoolInstanceProcs) {
          instance = proc(
            monitorBase,
            *poolContext.UserMgr,
            linkedBackends,
            *poolContext.ThreadPool,
            algoMetaStatistic,
            poolContext.MiningStats.get(),
            instanceConfig.Type,
            instanceConfig.Protocol,
            static_cast<unsigned>(instIdx),
            static_cast<unsigned>(instIdxE),
            instanceConfig.InstanceConfig,
            poolContext.PriceFetcher.get());
          if (instance)
            break;
        }

        if (!instance) {
          LOG_F(ERROR,
                "Can't create instance with type '%s' and prorotol '%s'",
                instanceConfig.Type.c_str(),
                instanceConfig.Protocol.c_str());
          return 1;
        }
      }

      for (PoolBackend *linkedBackend: linkedBackends)
        linkedBackend->getClientDispatcher().connectWith(instance);

      instance->start();
      poolContext.Instances[instIdx].reset(instance);
    }
  }

  // Start "complex mining stats"
  poolContext.MiningStats->start();

  // Start workers
  poolContext.ThreadPool->start();

  // Start user manager
  poolContext.UserMgr->start();

  // Start backends for all coins
  for (auto &backend: poolContext.Backends) {
    backend->start();
  }

  // Start algorithm meta statistic servers
  for (auto &server: poolContext.AlgoMetaStatistic) {
    server->start();
  }

  // Start clients polling
  for (auto &dispatcher: poolContext.ClientDispatchers) {
    dispatcher->poll();
  }

  poolContext.HttpServer.reset(new PoolHttpServer(poolContext.HttpPort, *poolContext.UserMgr, poolContext.Backends, poolContext.AlgoMetaStatistic, *poolContext.MiningStats, config, httpThreadsNum));
  poolContext.HttpServer->start();

  // Start monitor thread
  std::thread monitorThread([](asyncBase *base) {
    InitializeWorkerThread();
    loguru::set_thread_name("monitor");
    LOG_F(INFO, "monitor started tid=%u", GetGlobalThreadId());
    asyncLoop(base);
  }, monitorBase);

  // Handle CTRL+C (SIGINT)
  signal(SIGINT, sigIntHandler);
  signal(SIGTERM, sigIntHandler);
#ifndef WIN32
  signal(SIGUSR1, sigUsrHandler);
#endif

  std::thread sigIntThread([&monitorBase, &poolContext]() {
    loguru::set_thread_name("sigint_monitor");
    while (!interrupted) {
      if (sigusrReceived) {
        processSigUsr();
        sigusrReceived = 0;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    LOG_F(INFO, "Interrupted by user");
    // Stop mining stats
    if (poolContext.MiningStats)
      poolContext.MiningStats->stop();
    // Stop HTTP server
    poolContext.HttpServer->stop();
    // Stop workers
    poolContext.ThreadPool->stop();
    // Stop backends
    for (auto &backend: poolContext.Backends)
      backend->stop();
    for (auto &server: poolContext.AlgoMetaStatistic)
      server->stop();
    // Stop user manager
    poolContext.UserMgr->stop();

    // Stop monitor thread
    postQuitOperation(monitorBase);
  });

  sigIntThread.detach();
  monitorThread.join();
  LOG_F(INFO, "poolfrondend stopped\n");
  return 0;
}
