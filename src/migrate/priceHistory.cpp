#include "migratecommon.h"
#include "poolcommon/path.h"
#include "poolcore/coinLibrary.h"
#include "poolcore/plugin.h"
#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "asyncio/http.h"
#include "asyncio/socket.h"
#include "asyncio/socketSSL.h"
#include "p2putils/xmstream.h"
#include "rapidjson/document.h"
#include "loguru.hpp"

#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>

#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

extern CPluginContext gPluginContext;

static constexpr const char *CoinGeckoHost = "pro-api.coingecko.com";
static constexpr const char *CryptoCompareHost = "min-api.cryptocompare.com";

// CoinGecko ID → CryptoCompare ticker (fallback source)
static const std::unordered_map<std::string, std::string> CoinGeckoToCryptoCompare = {
  {"bitcoin",           "BTC"},
  {"litecoin",          "LTC"},
  {"ethereum-classic",  "ETC"},
  {"zcash",             "ZEC"},
  {"dash",              "DASH"},
  {"digibyte",          "DGB"},
  {"dogecoin",          "DOGE"},
  {"ecash",             "XEC"},
  {"primecoin",         "XPM"},
  {"komodo",            "KMD"},
  {"zencash",           "ZEN"},
  {"pirate-chain",      "ARRR"},
  {"bitcoin-cash",      "BCH"},
  {"bitcoin-cash-sv",   "BSV"},
  {"bellscoin",         "BEL"},
  {"litecoin-cash",     "LCC"},
  {"freecash",          "FCH"},
  {"fractal-bitcoin",   "FB"},
  {"pepecoin",          "PEPECOIN"},
  {"dingocoin",         "DINGO"},
  {"datacoin",          "DTC"},
  {"hootchain",         "HOOT"},
  {"junkcoin",          "JKC"},
  {"luckycoin",         "LKY"},
  {"osmium",            "OS76"},
  {"pepecoin-network",  "PEP"},
};

static constexpr uint64_t ConnectTimeoutUs = 15 * 1000000;
static constexpr uint64_t RequestTimeoutUs = 30 * 1000000;
static constexpr uint64_t RateLimitSleepUs = 65 * 1000000;
static constexpr uint64_t InterRequestSleepUs = 500000;
static constexpr uint64_t ErrorRetrySleepUs = 30 * 1000000;
static constexpr unsigned MaxRetries = 3;
static constexpr int64_t ChunkDurationSeconds = 90 * 24 * 3600;

struct CPriceHistoryContext {
  asyncBase *Base;
  HostAddress Address;
  HostAddress CryptoCompareAddress;
  aioUserEvent *SleepEvent;
  rocksdb::DB *Db;
  std::vector<std::string> CoinIds;
  std::unordered_map<std::string, int64_t> CoinStartTimestamp; // coinGeckoId → earliest payout
  std::string ApiKey;
  int64_t CoinGeckoEarliestTimestamp = 0; // 0 = no limit (Analyst+)
};

static int64_t partitionToTimestamp(const std::string &partName)
{
  int year = 0, month = 0;
  if (sscanf(partName.c_str(), "%d.%d", &year, &month) != 2)
    return 0;
  struct tm tm = {};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = 1;
  return timegm(&tm);
}

static std::string formatDate(int64_t timestamp)
{
  time_t t = static_cast<time_t>(timestamp);
  struct tm tm;
  gmtime_r(&t, &tm);
  return std::format("{:04d}-{:02d}-{:02d}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

static int64_t findEarliestPayoutTimestamp(const std::filesystem::path &coinPath)
{
  std::string earliestPartition;
  for (const char *dbName : {"payouts", "pplns.payouts"}) {
    std::filesystem::path dbPath = coinPath / dbName;
    if (!std::filesystem::is_directory(dbPath))
      continue;

    for (std::filesystem::directory_iterator PI(dbPath), PIE; PI != PIE; ++PI) {
      if (!is_directory(PI->status()))
        continue;
      std::string partName = PI->path().filename().generic_string();
      if (earliestPartition.empty() || partName < earliestPartition)
        earliestPartition = partName;
    }
  }

  return earliestPartition.empty() ? 0 : partitionToTimestamp(earliestPartition);
}

static std::vector<std::string> discoverCoins(const std::filesystem::path &sourceDatabase,
                                               std::unordered_map<std::string, int64_t> &startTimestamps)
{
  std::set<std::string> seen;
  std::vector<std::string> result;

  for (std::filesystem::directory_iterator I(sourceDatabase), IE; I != IE; ++I) {
    if (!is_directory(I->status()))
      continue;

    std::string name = I->path().filename().generic_string();
    if (name.starts_with("__"))
      continue;
    if (isNonCoinDirectory(name))
      continue;

    CCoinInfo info = CCoinLibrary::get(name.c_str());
    if (info.Name.empty()) {
      for (const auto &proc : gPluginContext.AddExtraCoinProcs) {
        if (proc(name.c_str(), info))
          break;
      }
    }

    if (info.Name.empty()) {
      CLOG_F(WARNING, "Unknown directory in database path: {}", name);
      continue;
    }

    if (info.IsAlgorithm)
      continue;

    if (info.CoinGeckoName.empty()) {
      CLOG_F(WARNING, "{}: no CoinGecko mapping, skipping", info.Name);
      continue;
    }

    int64_t earliest = findEarliestPayoutTimestamp(I->path());
    if (earliest == 0)
      continue;

    auto &ts = startTimestamps[info.CoinGeckoName];
    if (ts == 0 || earliest < ts)
      ts = earliest;

    if (seen.insert(info.CoinGeckoName).second) {
      CLOG_F(INFO, "Found coin: {} (CoinGecko: {}, earliest: {})", info.Name, info.CoinGeckoName, formatDate(earliest));
      result.push_back(info.CoinGeckoName);
    }
  }

  return result;
}

static std::string makeKey(const std::string &coinId, uint64_t timestamp)
{
  std::string key;
  key.reserve(coinId.size() + 1 + 8);
  key.append(coinId);
  key.push_back('\0');
  uint8_t buf[8];
  for (int i = 7; i >= 0; i--) {
    buf[i] = static_cast<uint8_t>(timestamp & 0xFF);
    timestamp >>= 8;
  }
  key.append(reinterpret_cast<const char *>(buf), 8);
  return key;
}

static int64_t getLastStoredTimestamp(rocksdb::DB *db, const std::string &coinId)
{
  std::string seekKey;
  seekKey.append(coinId);
  seekKey.push_back('\x01');

  rocksdb::ReadOptions readOptions;
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(readOptions));
  it->Seek(seekKey);
  if (!it->Valid())
    it->SeekToLast();
  else
    it->Prev();

  if (!it->Valid())
    return 0;

  rocksdb::Slice key = it->key();
  const char *data = key.data();
  size_t size = key.size();

  if (size != coinId.size() + 1 + 8)
    return 0;
  if (memcmp(data, coinId.data(), coinId.size()) != 0 || data[coinId.size()] != '\0')
    return 0;

  const uint8_t *ts = reinterpret_cast<const uint8_t *>(data + coinId.size() + 1);
  uint64_t timestamp = 0;
  for (int i = 0; i < 8; i++)
    timestamp = (timestamp << 8) | ts[i];

  return static_cast<int64_t>(timestamp);
}

static void buildGetQuery(const char *host, const std::string &path, xmstream &out)
{
  out.reset();
  out.write("GET ");
  out.write(path.data(), path.size());
  out.write(" HTTP/1.1\r\n");
  out.write("Host: ");
  out.write(host, strlen(host));
  out.write("\r\n");
  out.write("User-Agent: poolcore/1.0\r\n");
  out.write("Accept: application/json\r\n");
  out.write("\r\n");
}

enum class ERequestResult {
  Success,
  RateLimited,
  Error
};

static ERequestResult fetchChunk(
  CPriceHistoryContext *ctx,
  const std::string &coinId,
  int64_t fromTimestamp,
  int64_t toTimestamp,
  std::vector<std::pair<int64_t, double>> &prices)
{
  std::string path = std::format(
    "/api/v3/coins/{}/market_chart/range?vs_currency=usd&precision=full&from={}&to={}&x_cg_pro_api_key={}",
    coinId, fromTimestamp, toTimestamp, ctx->ApiKey);

  xmstream query;
  buildGetQuery(CoinGeckoHost, path, query);

  SSLSocket *sslSocket = sslSocketNew(ctx->Base, nullptr);
  HTTPClient *client = httpsClientNew(ctx->Base, sslSocket);
  HTTPParseDefaultContext parseCtx;
  httpParseDefaultInit(&parseCtx);

  int connectResult = ioHttpConnect(client, &ctx->Address, CoinGeckoHost, ConnectTimeoutUs);
  if (connectResult != 0) {
    CLOG_F(ERROR, "  {}: connect failed", coinId);
    httpClientDelete(client);
    return ERequestResult::Error;
  }

  AsyncOpStatus requestStatus = ioHttpRequest(
    client,
    query.data<const char>(),
    query.sizeOf(),
    RequestTimeoutUs,
    httpParseDefault,
    &parseCtx);

  unsigned httpCode = parseCtx.resultCode;

  if (requestStatus != aosSuccess) {
    CLOG_F(ERROR, "  {}: request failed, status={}", coinId, static_cast<unsigned>(requestStatus));
    httpClientDelete(client);
    return ERequestResult::Error;
  }

  httpClientDelete(client);

  if (httpCode == 429) {
    CLOG_F(WARNING, "  {}: rate limited (429)", coinId);
    return ERequestResult::RateLimited;
  }

  if (httpCode != 200) {
    CLOG_F(ERROR, "  {}: HTTP {}", coinId, httpCode);
    return ERequestResult::Error;
  }

  rapidjson::Document document;
  document.Parse(parseCtx.body.data, parseCtx.body.size);
  if (document.HasParseError() || !document.IsObject()) {
    CLOG_F(ERROR, "  {}: JSON parse error", coinId);
    return ERequestResult::Error;
  }

  if (!document.HasMember("prices") || !document["prices"].IsArray()) {
    CLOG_F(ERROR, "  {}: missing 'prices' array", coinId);
    return ERequestResult::Error;
  }

  const auto &pricesArray = document["prices"].GetArray();
  prices.clear();
  prices.reserve(pricesArray.Size());

  for (rapidjson::SizeType i = 0; i < pricesArray.Size(); i++) {
    if (!pricesArray[i].IsArray() || pricesArray[i].Size() != 2)
      continue;
    if (!pricesArray[i][0].IsNumber() || !pricesArray[i][1].IsNumber())
      continue;
    int64_t timestampSec = pricesArray[i][0].GetInt64() / 1000;
    double price = pricesArray[i][1].GetDouble();
    prices.emplace_back(timestampSec, price);
  }

  return ERequestResult::Success;
}

static ERequestResult fetchChunkCryptoCompare(
  CPriceHistoryContext *ctx,
  const std::string &ticker,
  int64_t fromTimestamp,
  int64_t toTimestamp,
  std::vector<std::pair<int64_t, double>> &prices)
{
  int64_t days = (toTimestamp - fromTimestamp) / 86400 + 1;
  std::string path = std::format("/data/v2/histoday?fsym={}&tsym=USD&toTs={}&limit={}", ticker, toTimestamp, days);

  xmstream query;
  buildGetQuery(CryptoCompareHost, path, query);

  SSLSocket *sslSocket = sslSocketNew(ctx->Base, nullptr);
  HTTPClient *client = httpsClientNew(ctx->Base, sslSocket);
  HTTPParseDefaultContext parseCtx;
  httpParseDefaultInit(&parseCtx);

  int connectResult = ioHttpConnect(client, &ctx->CryptoCompareAddress, CryptoCompareHost, ConnectTimeoutUs);
  if (connectResult != 0) {
    CLOG_F(ERROR, "  {}: CryptoCompare connect failed", ticker);
    httpClientDelete(client);
    return ERequestResult::Error;
  }

  AsyncOpStatus requestStatus = ioHttpRequest(
    client,
    query.data<const char>(),
    query.sizeOf(),
    RequestTimeoutUs,
    httpParseDefault,
    &parseCtx);

  unsigned httpCode = parseCtx.resultCode;

  if (requestStatus != aosSuccess) {
    CLOG_F(ERROR, "  {}: CryptoCompare request failed, status={}", ticker, static_cast<unsigned>(requestStatus));
    httpClientDelete(client);
    return ERequestResult::Error;
  }

  httpClientDelete(client);

  if (httpCode != 200) {
    CLOG_F(ERROR, "  {}: CryptoCompare HTTP {}", ticker, httpCode);
    return ERequestResult::Error;
  }

  rapidjson::Document document;
  document.Parse(parseCtx.body.data, parseCtx.body.size);
  if (document.HasParseError() || !document.IsObject()) {
    CLOG_F(ERROR, "  {}: CryptoCompare JSON parse error", ticker);
    return ERequestResult::Error;
  }

  if (!document.HasMember("Response") || !document["Response"].IsString() ||
      strcmp(document["Response"].GetString(), "Success") != 0) {
    CLOG_F(ERROR, "  {}: CryptoCompare API error", ticker);
    return ERequestResult::Error;
  }

  if (!document.HasMember("Data") || !document["Data"].IsObject()) {
    CLOG_F(ERROR, "  {}: CryptoCompare missing Data object", ticker);
    return ERequestResult::Error;
  }

  const auto &dataObj = document["Data"];
  if (!dataObj.HasMember("Data") || !dataObj["Data"].IsArray()) {
    CLOG_F(ERROR, "  {}: CryptoCompare missing Data.Data array", ticker);
    return ERequestResult::Error;
  }

  const auto &dataArray = dataObj["Data"].GetArray();
  prices.clear();
  prices.reserve(dataArray.Size());

  for (rapidjson::SizeType i = 0; i < dataArray.Size(); i++) {
    const auto &entry = dataArray[i];
    if (!entry.IsObject())
      continue;
    if (!entry.HasMember("time") || !entry["time"].IsNumber())
      continue;
    if (!entry.HasMember("close") || !entry["close"].IsNumber())
      continue;

    int64_t timestamp = entry["time"].GetInt64();
    double price = entry["close"].GetDouble();
    if (price > 0.0 && timestamp >= fromTimestamp)
      prices.emplace_back(timestamp, price);
  }

  return ERequestResult::Success;
}

// Probe CoinGecko with a 3-year-old request to detect plan limits.
// Returns earliest available timestamp (now - 2 years for Basic plan), or 0 if no limit (Analyst+).
static int64_t probeCoinGeckoHistoryLimit(CPriceHistoryContext *ctx)
{
  int64_t now = static_cast<int64_t>(time(nullptr));
  int64_t probeFrom = now - 3LL * 365 * 86400;
  int64_t probeTo = probeFrom + 86400;

  std::string path = std::format(
    "/api/v3/coins/bitcoin/market_chart/range?vs_currency=usd&from={}&to={}&x_cg_pro_api_key={}",
    probeFrom, probeTo, ctx->ApiKey);

  xmstream query;
  buildGetQuery(CoinGeckoHost, path, query);

  SSLSocket *sslSocket = sslSocketNew(ctx->Base, nullptr);
  HTTPClient *client = httpsClientNew(ctx->Base, sslSocket);
  HTTPParseDefaultContext parseCtx;
  httpParseDefaultInit(&parseCtx);

  int connectResult = ioHttpConnect(client, &ctx->Address, CoinGeckoHost, ConnectTimeoutUs);
  if (connectResult != 0) {
    CLOG_F(WARNING, "CoinGecko probe: connect failed, assuming 2-year limit");
    httpClientDelete(client);
    return now - 2LL * 365 * 86400;
  }

  AsyncOpStatus requestStatus = ioHttpRequest(
    client,
    query.data<const char>(),
    query.sizeOf(),
    RequestTimeoutUs,
    httpParseDefault,
    &parseCtx);

  unsigned httpCode = parseCtx.resultCode;
  httpClientDelete(client);

  if (requestStatus != aosSuccess) {
    CLOG_F(WARNING, "CoinGecko probe: request failed, assuming 2-year limit");
    return now - 2LL * 365 * 86400;
  }

  if (httpCode == 200) {
    CLOG_F(INFO, "CoinGecko probe: full history available (Analyst+ plan)");
    return 0;
  }

  CLOG_F(INFO, "CoinGecko probe: HTTP {}, limited plan — will use CryptoCompare for data older than 2 years", httpCode);
  return now - 2LL * 365 * 86400;
}

static bool writePriceBatch(
  rocksdb::DB *db,
  const std::string &coinId,
  const std::vector<std::pair<int64_t, double>> &prices)
{
  if (prices.empty())
    return true;

  rocksdb::WriteBatch batch;
  for (const auto &[timestamp, price] : prices) {
    std::string key = makeKey(coinId, static_cast<uint64_t>(timestamp));
    batch.Put(key, rocksdb::Slice(reinterpret_cast<const char *>(&price), sizeof(double)));
  }

  rocksdb::WriteOptions writeOptions;
  rocksdb::Status status = db->Write(writeOptions, &batch);
  if (!status.ok()) {
    CLOG_F(ERROR, "  {}: RocksDB write failed: {}", coinId, status.ToString());
    return false;
  }

  return true;
}

static bool downloadCoinHistory(CPriceHistoryContext *ctx, const std::string &coinId)
{
  CLOG_F(INFO, "{}: starting download", coinId);

  int64_t lastStored = getLastStoredTimestamp(ctx->Db, coinId);
  int64_t startTime;
  if (lastStored > 0) {
    startTime = lastStored + 1;
    CLOG_F(INFO, "  {}: resuming from {}", coinId, formatDate(startTime));
  } else {
    auto it = ctx->CoinStartTimestamp.find(coinId);
    if (it == ctx->CoinStartTimestamp.end()) {
      CLOG_F(WARNING, "  {}: no payout data found, skipping", coinId);
      return true;
    }
    startTime = it->second;
    CLOG_F(INFO, "  {}: starting from {} (no previous data)", coinId, formatDate(startTime));
  }

  int64_t now = static_cast<int64_t>(time(nullptr));
  if (startTime >= now) {
    CLOG_F(INFO, "  {}: already up to date", coinId);
    return true;
  }

  int64_t chunkStart = startTime;
  unsigned totalPoints = 0;

  while (chunkStart < now) {
    int64_t chunkEnd = std::min(chunkStart + ChunkDurationSeconds, now);

    std::vector<std::pair<int64_t, double>> prices;
    bool chunkDone = false;
    unsigned rateLimitCount = 0;

    bool useCryptoCompare = ctx->CoinGeckoEarliestTimestamp > 0 && chunkStart < ctx->CoinGeckoEarliestTimestamp;
    if (useCryptoCompare) {
      auto tickerIt = CoinGeckoToCryptoCompare.find(coinId);
      if (tickerIt != CoinGeckoToCryptoCompare.end()) {
        ERequestResult result = fetchChunkCryptoCompare(ctx, tickerIt->second, chunkStart, chunkEnd, prices);
        if (result == ERequestResult::Success)
          chunkDone = true;
      }
    } else {
      for (unsigned retry = 0; retry < MaxRetries && !chunkDone; retry++) {
        ERequestResult result = fetchChunk(ctx, coinId, chunkStart, chunkEnd, prices);

        switch (result) {
          case ERequestResult::Success:
            chunkDone = true;
            break;
          case ERequestResult::RateLimited:
            rateLimitCount++;
            if (rateLimitCount > 10) {
              CLOG_F(ERROR, "  {}: too many rate limits, aborting", coinId);
              return false;
            }
            CLOG_F(INFO, "  {}: sleeping 65s (rate limit)", coinId);
            ioSleep(ctx->SleepEvent, RateLimitSleepUs);
            retry--;
            break;
          case ERequestResult::Error:
            if (retry + 1 < MaxRetries) {
              CLOG_F(WARNING,
                "  {}: error, retrying in 30s (attempt {}/{})",
                coinId,
                retry + 1,
                MaxRetries);
              ioSleep(ctx->SleepEvent, ErrorRetrySleepUs);
            }
            break;
        }
      }
    }

    if (!chunkDone) {
      CLOG_F(ERROR,
        "  {}: failed for chunk {}..{} (source: {})",
        coinId,
        formatDate(chunkStart),
        formatDate(chunkEnd),
        useCryptoCompare ? "CryptoCompare" : "CoinGecko");
      return false;
    }

    if (!writePriceBatch(ctx->Db, coinId, prices))
      return false;

    totalPoints += static_cast<unsigned>(prices.size());
    CLOG_F(INFO,
      "  {}: fetched {}..{} ({} points, {})",
      coinId,
      formatDate(chunkStart),
      formatDate(chunkEnd),
      prices.size(),
      useCryptoCompare ? "CryptoCompare" : "CoinGecko");

    chunkStart = chunkEnd;

    if (chunkStart < now)
      ioSleep(ctx->SleepEvent, InterRequestSleepUs);
  }

  CLOG_F(INFO, "{}: completed, {} total points stored", coinId, totalPoints);
  return true;
}

static void priceHistoryCoroutine(void *arg)
{
  CPriceHistoryContext *ctx = static_cast<CPriceHistoryContext *>(arg);

  ctx->CoinGeckoEarliestTimestamp = probeCoinGeckoHistoryLimit(ctx);

  bool allOk = true;

  for (const auto &coinId : ctx->CoinIds) {
    if (!downloadCoinHistory(ctx, coinId)) {
      CLOG_F(ERROR, "{}: failed, continuing with next coin", coinId);
      allOk = false;
    }
  }

  if (allOk)
    CLOG_F(INFO, "All coins completed successfully");
  else
    CLOG_F(WARNING, "Some coins had errors, check log above");

  postQuitOperation(ctx->Base);
}

bool fetchPriceHistory(const char *sourceDatabase, const char *dbPath, const char *apiKey)
{
  CLOG_F(INFO, "Fetching price history to {}", dbPath);
  CLOG_F(INFO, "Discovering coins from {}", sourceDatabase);

  std::unordered_map<std::string, int64_t> coinStartTimestamps;
  std::vector<std::string> coinIds = discoverCoins(sourceDatabase, coinStartTimestamps);
  if (coinIds.empty()) {
    CLOG_F(ERROR, "No coins with payout data found in {}", sourceDatabase);
    return false;
  }

  CLOG_F(INFO, "Will fetch price history for {} coins", coinIds.size());

  rocksdb::Options options;
  options.create_if_missing = true;
  options.compression = rocksdb::kZSTD;
  rocksdb::DB *rawDb = nullptr;
  rocksdb::Status status = rocksdb::DB::Open(options, dbPath, &rawDb);
  if (!status.ok()) {
    CLOG_F(ERROR, "Can't open database {}: {}", dbPath, status.ToString());
    return false;
  }
  std::unique_ptr<rocksdb::DB> db(rawDb);

  initializeSocketSubsystem();
  asyncBase *base = createAsyncBase(amOSDefault);

  HostAddress address;
  {
    struct hostent *host = gethostbyname(CoinGeckoHost);
    if (!host) {
      CLOG_F(ERROR, "Can't resolve {}", CoinGeckoHost);
      return false;
    }
    struct in_addr **addrList = reinterpret_cast<struct in_addr **>(host->h_addr_list);
    if (!addrList[0]) {
      CLOG_F(ERROR, "No addresses for {}", CoinGeckoHost);
      return false;
    }
    address.ipv4 = addrList[0]->s_addr;
    address.port = htons(443);
    address.family = AF_INET;
  }

  CLOG_F(INFO, "Resolved {} successfully", CoinGeckoHost);

  HostAddress cryptoCompareAddress;
  {
    struct hostent *host = gethostbyname(CryptoCompareHost);
    if (!host) {
      CLOG_F(ERROR, "Can't resolve {}", CryptoCompareHost);
      return false;
    }
    struct in_addr **addrList = reinterpret_cast<struct in_addr **>(host->h_addr_list);
    if (!addrList[0]) {
      CLOG_F(ERROR, "No addresses for {}", CryptoCompareHost);
      return false;
    }
    cryptoCompareAddress.ipv4 = addrList[0]->s_addr;
    cryptoCompareAddress.port = htons(443);
    cryptoCompareAddress.family = AF_INET;
  }

  CLOG_F(INFO, "Resolved {} successfully", CryptoCompareHost);

  CPriceHistoryContext ctx;
  ctx.Base = base;
  ctx.Address = address;
  ctx.CryptoCompareAddress = cryptoCompareAddress;
  ctx.Db = db.get();
  ctx.CoinIds = std::move(coinIds);
  ctx.CoinStartTimestamp = std::move(coinStartTimestamps);
  ctx.ApiKey = apiKey;
  ctx.SleepEvent = newUserEvent(base, 0, nullptr, nullptr);

  coroutineCall(coroutineNew([](void *arg) {
    priceHistoryCoroutine(arg);
  }, &ctx, 0x100000));

  asyncLoop(base);

  deleteUserEvent(ctx.SleepEvent);
  CLOG_F(INFO, "Price history fetch completed");
  return true;
}

double CPriceDatabase::lookupPrice(const std::string &coinGeckoId, int64_t timestamp) const
{
  auto coinIt = Prices.find(coinGeckoId);
  if (coinIt == Prices.end() || coinIt->second.empty())
    return 0.0;

  const auto &m = coinIt->second;
  auto it = m.lower_bound(timestamp);

  if (it == m.end())
    return std::prev(it)->second;
  if (it->first == timestamp)
    return it->second;
  if (it == m.begin())
    return it->second;

  auto prev = std::prev(it);
  return (timestamp - prev->first <= it->first - timestamp) ? prev->second : it->second;
}

bool loadPriceDatabase(const std::filesystem::path &dbPath, CPriceDatabase &out)
{
  CLOG_F(INFO, "Loading price database from {}", dbPath);

  rocksdb::Options options;
  options.create_if_missing = false;
  rocksdb::DB *rawDb = nullptr;
  std::string dbPathStr = dbPath.generic_string();
  rocksdb::Status status = rocksdb::DB::Open(options, dbPathStr, &rawDb);
  if (!status.ok()) {
    CLOG_F(ERROR, "Can't open price database {}: {}", dbPathStr, status.ToString());
    return false;
  }
  std::unique_ptr<rocksdb::DB> db(rawDb);

  unsigned totalRecords = 0;
  rocksdb::ReadOptions readOptions;
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(readOptions));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    rocksdb::Slice key = it->key();
    rocksdb::Slice value = it->value();

    // Key format: coinGeckoId\0 + 8-byte big-endian timestamp
    const char *keyData = key.data();
    size_t keySize = key.size();

    // Find the \0 separator
    const char *sep = static_cast<const char *>(memchr(keyData, '\0', keySize));
    if (!sep || keyData + keySize - sep - 1 != 8)
      continue;

    std::string coinId(keyData, sep - keyData);

    const uint8_t *ts = reinterpret_cast<const uint8_t *>(sep + 1);
    int64_t timestamp = 0;
    for (int i = 0; i < 8; i++)
      timestamp = (timestamp << 8) | ts[i];

    if (value.size() != sizeof(double))
      continue;

    double price;
    memcpy(&price, value.data(), sizeof(double));

    out.Prices[coinId][timestamp] = price;
    totalRecords++;
  }

  CLOG_F(INFO, "Loaded {} price records for {} coins", totalRecords, out.Prices.size());
  return true;
}
