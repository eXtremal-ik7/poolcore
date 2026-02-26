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
#include <set>
#include <string>
#include <vector>

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

extern CPluginContext gPluginContext;

static constexpr const char *CoinGeckoHost = "pro-api.coingecko.com";
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
  aioUserEvent *SleepEvent;
  rocksdb::DB *Db;
  std::vector<std::string> CoinIds;
  std::string ApiKey;
  int64_t StartTimestamp;
};

static std::vector<std::string> discoverCoinGeckoIds(const std::filesystem::path &sourceDatabase)
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

    if (seen.insert(info.CoinGeckoName).second) {
      CLOG_F(INFO, "Found coin: {} (CoinGecko: {})", info.Name, info.CoinGeckoName);
      result.push_back(info.CoinGeckoName);
    }
  }

  return result;
}

static int64_t findEarliestPayoutTimestamp(const std::filesystem::path &sourceDatabase)
{
  std::string earliestPartition;

  for (std::filesystem::directory_iterator I(sourceDatabase), IE; I != IE; ++I) {
    if (!is_directory(I->status()))
      continue;

    std::string name = I->path().filename().generic_string();
    if (name.starts_with("__"))
      continue;
    if (isNonCoinDirectory(name))
      continue;

    // Old format: pplns.payouts, new format: pplns.payouts.2
    std::filesystem::path payoutsPath;
    if (std::filesystem::is_directory(I->path() / "pplns.payouts"))
      payoutsPath = I->path() / "pplns.payouts";
    else
      continue;

    for (std::filesystem::directory_iterator PI(payoutsPath), PIE; PI != PIE; ++PI) {
      if (!is_directory(PI->status()))
        continue;
      std::string partName = PI->path().filename().generic_string();
      if (earliestPartition.empty() || partName < earliestPartition)
        earliestPartition = partName;
    }
  }

  if (earliestPartition.empty())
    return 0;

  int year = 0, month = 0;
  if (sscanf(earliestPartition.c_str(), "%d.%d", &year, &month) != 2)
    return 0;

  struct tm tm = {};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = 1;
  return timegm(&tm);
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

static void buildGetQuery(const std::string &path, xmstream &out)
{
  out.reset();
  out.write("GET ");
  out.write(path.data(), path.size());
  out.write(" HTTP/1.1\r\n");
  out.write("Host: ");
  out.write(CoinGeckoHost, strlen(CoinGeckoHost));
  out.write("\r\n");
  out.write("User-Agent: poolcore/1.0\r\n");
  out.write("Accept: application/json\r\n");
  out.write("\r\n");
}

static std::string formatDate(int64_t timestamp)
{
  time_t t = static_cast<time_t>(timestamp);
  struct tm tm;
  gmtime_r(&t, &tm);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return buf;
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
  char path[768];
  snprintf(
    path,
    sizeof(path),
    "/api/v3/coins/%s/market_chart/range?vs_currency=usd&precision=full&from=%lld&to=%lld&x_cg_pro_api_key=%s",
    coinId.c_str(),
    static_cast<long long>(fromTimestamp),
    static_cast<long long>(toTimestamp),
    ctx->ApiKey.c_str());

  xmstream query;
  buildGetQuery(path, query);

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
    startTime = ctx->StartTimestamp;
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

    if (!chunkDone) {
      CLOG_F(ERROR,
        "  {}: failed after {} retries for chunk {}..{}",
        coinId,
        MaxRetries,
        formatDate(chunkStart),
        formatDate(chunkEnd));
      return false;
    }

    if (!writePriceBatch(ctx->Db, coinId, prices))
      return false;

    totalPoints += static_cast<unsigned>(prices.size());
    CLOG_F(INFO,
      "  {}: fetched {}..{} ({} points)",
      coinId,
      formatDate(chunkStart),
      formatDate(chunkEnd),
      prices.size());

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

  std::vector<std::string> coinIds = discoverCoinGeckoIds(sourceDatabase);
  if (coinIds.empty()) {
    CLOG_F(ERROR, "No coins with CoinGecko mapping found in {}", sourceDatabase);
    return false;
  }

  int64_t startTimestamp = findEarliestPayoutTimestamp(sourceDatabase);
  if (startTimestamp == 0) {
    CLOG_F(ERROR, "No pplns.payouts partitions found in {}", sourceDatabase);
    return false;
  }

  CLOG_F(INFO, "Earliest payout partition: {}", formatDate(startTimestamp));

  // CoinGecko Basic plan: historical data limited to past 2 years
  static constexpr int64_t TwoYearsSeconds = 2 * 365 * 24 * 3600;
  int64_t now = static_cast<int64_t>(time(nullptr));
  int64_t twoYearsAgo = now - TwoYearsSeconds;
  if (startTimestamp < twoYearsAgo) {
    CLOG_F(WARNING,
      "Start date {} is beyond CoinGecko 2-year limit, clamping to {}",
      formatDate(startTimestamp),
      formatDate(twoYearsAgo));
    startTimestamp = twoYearsAgo;
  }

  CLOG_F(INFO, "Will fetch price history for {} coins from {}", coinIds.size(), formatDate(startTimestamp));

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

  CPriceHistoryContext ctx;
  ctx.Base = base;
  ctx.Address = address;
  ctx.Db = db.get();
  ctx.CoinIds = std::move(coinIds);
  ctx.ApiKey = apiKey;
  ctx.StartTimestamp = startTimestamp;
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
