#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "asyncio/socket.h"
#include "p2p/p2p.h"
#include "p2putils/uriParse.h"
#include "poolcommon/poolapi.h"
#include "loguru.hpp"
#include <stdio.h>
#include <string.h>
#include <chrono>

// TODO: move to another place
static const int64_t COIN = 100000000;

struct PoolRpcCmdContext {
  HostAddress address;
  p2pNode *client;
  const char *methodName;
  int argc;
  char **argv;
  bool result;
};

typedef bool methodProcTy(PoolRpcCmdContext*);

struct MethodMapElement {
  const char *name;
  methodProcTy *proc;
};

bool getInfoProc(PoolRpcCmdContext *context);
bool getCurrentBlock(PoolRpcCmdContext *context);
bool getBlockTemplate(PoolRpcCmdContext *context);
bool sendProofOfWork(PoolRpcCmdContext *context);
bool getBlockByHash(PoolRpcCmdContext *context);
bool getBalance(PoolRpcCmdContext *context);
bool sendMoney(PoolRpcCmdContext *context);

bool ZGetBalance(PoolRpcCmdContext *context);
bool ZSendMoney(PoolRpcCmdContext *context);
bool listUnspent(PoolRpcCmdContext *context);
bool ZAsyncOperationStatus(PoolRpcCmdContext *context);

bool queryFoundBlocks(PoolRpcCmdContext *context);
bool queryClientInfo(PoolRpcCmdContext *context);
bool queryPoolBalance(PoolRpcCmdContext *context);
bool queryPayouts(PoolRpcCmdContext *context);
bool queryClientStats(PoolRpcCmdContext *context);
bool queryPoolStats(PoolRpcCmdContext *context);
bool updateClientInfo(PoolRpcCmdContext *context);

bool resendBrokenTx(PoolRpcCmdContext *context);
bool moveBalance(PoolRpcCmdContext *context);
bool manualPayout(PoolRpcCmdContext *context);

static MethodMapElement methodMap[] = {
  {"getInfo", getInfoProc},
  {"getCurrentBlock", getCurrentBlock},
  {"getBlockTemplate", getBlockTemplate},
  {"sendProofOfWork", sendProofOfWork},
  {"getBlockByHash", getBlockByHash},
  {"getBalance", getBalance},
  {"sendMoney", sendMoney},
  {"z_getBalance", ZGetBalance},
  {"z_sendMoney", ZSendMoney},
  {"listUnspent", listUnspent},
  {"z_asyncOperationStatus", ZAsyncOperationStatus},
  
  {"queryFoundBlocks", queryFoundBlocks},
  {"queryPoolBalance", queryPoolBalance},
  {"queryPayouts", queryPayouts},
  {"queryClientStats", queryClientStats},
  {"queryPoolStats", queryPoolStats},  
  {"queryClientInfo", queryClientInfo},
  {"updateClientInfo", updateClientInfo},
  
  {"resendBrokenTx", resendBrokenTx},
  {"moveBalance", moveBalance},
  {"manualPayout", manualPayout},  
  {0, 0}
};

static const char *unitType(UnitType type)
{
  static const char *types[] = {
    "CPU",
    "GPU",
    "ASIC",
    "OTHER"
  };
  
  return types[std::min(type, UnitType_OTHER)];
}

static const char *asyncOpState(AsyncOpState state)
{
  static const char *states[] = {
    "ready",
    "executing",
    "cancelled",
    "failed",
    "success"
  };
  
  return states[state];
}

bool getInfoProc(PoolRpcCmdContext *context)
{   
  auto info = ioGetInfo(context->client);
  if (!info)
    return false;
  LOG_F(INFO, " * coin: %s", info->coin.c_str());
  return true;
}


bool getCurrentBlock(PoolRpcCmdContext *context)
{
  auto block = ioGetCurrentBlock(context->client);
  if (!block)
    return false;
  LOG_F(INFO, " * height: %llu", block->height);
  LOG_F(INFO, " * nbits: %llu", block->bits);
  LOG_F(INFO, " * hash: %s", !block->hash.empty() ? block->hash.c_str() : "<empty>");
  LOG_F(INFO, " * prevhash: %s", !block->prevhash.empty() ? block->prevhash.c_str() : "<empty>");
  if (!block->hashreserved.empty())
    LOG_F(INFO, " * hashreserved: %s", block->hashreserved.c_str());
  LOG_F(INFO, " * time: %u", (unsigned)block->time);
  return true;
}

bool getBlockTemplate(PoolRpcCmdContext *context)
{
  auto beginPt = std::chrono::steady_clock::now();  
  auto block = ioGetBlockTemplate(context->client);
  if (!block)
    return false;
  auto endPt = std::chrono::steady_clock::now(); 
  auto callTime = std::chrono::duration_cast<std::chrono::microseconds>(endPt-beginPt).count() / 1000.0;
  LOG_F(INFO, "getBlockTemplate call duration %.3lfms", callTime);
  
  LOG_F(INFO, " * nbits: %llu", block->bits);
  LOG_F(INFO, " * prevhash: %s", !block->prevhash.empty() ? block->prevhash.c_str() : "<empty>");
  if (!block->hashreserved.empty())
    LOG_F(INFO, " * hashreserved: %s", block->hashreserved.c_str());
  LOG_F(INFO, " * merkle: %s", !block->merkle.empty() ? block->merkle.c_str() : "<empty>");
  LOG_F(INFO, " * time: %u", (unsigned)block->time);
  LOG_F(INFO, " * extraNonce: %u", (unsigned)block->extraNonce);
  if (block->equilHashK != -1)
    LOG_F(INFO, " * equilHashK = %i", block->equilHashK);
  if (block->equilHashN != -1)
    LOG_F(INFO, " * equilHashN = %i", block->equilHashN);
  return true;
}

bool sendProofOfWork(PoolRpcCmdContext *context)
{
  if (context->argc != 5) {
    LOG_F(ERROR, "Usage: sendProofOfWork <height> <time> <nonce> <extraNonce> <data>");
    return false;
  }

  auto result = ioSendProofOfWork(context->client,
                                  xatoi<int64_t>(context->argv[0]),
                                  xatoi<int64_t>(context->argv[1]),
                                  context->argv[2],
                                  xatoi<int64_t>(context->argv[3]),
                                  context->argv[4]);
  if (!result)
    return false;

  if (result->result == false) {
    LOG_F(INFO, " * proofOfWork check failed");
  } else {
    LOG_F(INFO, " * proofOfWork accepted!");
    LOG_F(INFO, "   * generated: %lli coins", result->generatedCoins);
  }

  return true;
}

bool getBlockByHash(PoolRpcCmdContext *context)
{
  if (context->argc != 1) {
    LOG_F(ERROR, "Usage: getBlockByHash <hash>");
    return false;
  }

  std::vector<std::string> hashVector = {context->argv[0]};
  auto result = ioGetBlockByHash(context->client, hashVector);
  if (!result)
    return false;

  for (size_t i = 0; i < result->blocks.size(); i++) {
    LOG_F(INFO, " * block %u", (unsigned)i);
    
    auto &block = result->blocks[i];
    LOG_F(INFO, "   * height: %llu", block->height);
    LOG_F(INFO, "   * nbits: %llu", block->bits);
    LOG_F(INFO, "   * hash: %s", !block->hash.empty() ? block->hash.c_str() : "<empty>");
    LOG_F(INFO, "   * prevhash: %s", !block->prevhash.empty() ? block->prevhash.c_str() : "<empty>");
    if (!block->hashreserved.empty())
      LOG_F(INFO, "   * hashreserved: %s", block->hashreserved.c_str());
    LOG_F(INFO, "   * merkle: %s", !block->merkle.empty() ? block->merkle.c_str() : "<empty>");
    LOG_F(INFO, "   * time: %u", (unsigned)block->time);
    LOG_F(INFO, "   * confirmations: %i", (int)block->confirmations);
  }

  return true;
}

bool getBalance(PoolRpcCmdContext *context)
{
  if (context->argc != 0) {
    LOG_F(ERROR, "Usage: getBalance");
    return false;
  }

  auto result = ioGetBalance(context->client);
  if (!result)
    return false;
  
  LOG_F(INFO, " * balance: %lli", result->balance);
  LOG_F(INFO, " * immature: %lli", result->immature);
  return true;
}


bool sendMoney(PoolRpcCmdContext *context)
{
  if (context->argc != 2) {
    LOG_F(ERROR, "Usage: sendMoney <destination> <amount>");
    return false;
  }
  
  int64_t amount = atof(context->argv[1])*COIN;
  auto result = ioSendMoney(context->client, context->argv[0], amount);
  if (!result)
    return false;
  
  LOG_F(INFO, " * send money is %s", result->success ? "OK" : "FAILED");
  if (result->success)
    LOG_F(INFO, " * transaction ID: %s", result->txid.c_str());
  else
    LOG_F(INFO, " * error: %s", result->error.c_str());
  LOG_F(INFO, " * fee: %lli", result->fee);
  LOG_F(INFO, " * remaining: %lli", result->remaining);
  return true;
}

bool ZGetBalance(PoolRpcCmdContext *context)
{
  if (context->argc != 1) {
    LOG_F(ERROR, "Usage: z_getBalance <address>\n  address - your z-addr or t-addr");
    return false;
  }
  
  auto result = ioZGetBalance(context->client, context->argv[0]);
  if (!result)
    return false;
  
  if (result->balance != -1) {
    LOG_F(INFO, " * balance of %s: %lu", context->argv[0], result->balance);
  } else {
    LOG_F(INFO, "<error> %s", result->error.c_str());
  }

  return true;
}

bool ZSendMoney(PoolRpcCmdContext *context)
{
  if (context->argc != 4) {
    LOG_F(ERROR, "Usage: z_sendMoney <source> <destination> <amount> <memo>");
    return false;
  }
  
  ZDestinationT singleDestination;
  singleDestination.address = context->argv[1];
  singleDestination.amount = xatoi<int64_t>(context->argv[2]);
  singleDestination.memo = context->argv[3];
  
  auto result = ioZSendMoney(context->client, context->argv[0], { singleDestination })  ;
  if (!result)
    return false;
  
  if (!result->asyncOperationId.empty()) {
    LOG_F(INFO, " * async operation id: %s", result->asyncOperationId.c_str());
  } else {
    LOG_F(INFO, "<error> %s", result->error.c_str());
  }

  return true;
}

bool listUnspent(PoolRpcCmdContext *context)
{
  if (context->argc != 0) {
    LOG_F(ERROR, "Usage: listUnspent");
    return false;
  }
  
  auto result = ioListUnspent(context->client);
  if (!result)
    return false;
  
  for (size_t i = 0; i < result->outs.size(); i++) {
    LOG_F(INFO, " * out %u", (unsigned)i);
    
    auto &out = result->outs[i];
    LOG_F(INFO, "   * address: %s", out->address.c_str());
    LOG_F(INFO, "   * amount: %lu", (unsigned long)out->amount);
    LOG_F(INFO, "   * confirmations: %i", (int)out->confirmations);
    LOG_F(INFO, "   * spendable: %s", out->spendable ? "true" : "false");
  }

  return true;
}

bool ZAsyncOperationStatus(PoolRpcCmdContext *context)
{
  if (context->argc != 1) {
    LOG_F(ERROR, "Usage: z_asyncOperationStatus <asyncOpId>");
    return false;
  }
  
  auto result = ioAsyncOperationStatus(context->client, context->argv[0]);
  if (!result)
    return false;
  
  LOG_F(INFO, " * operations number: %u", (unsigned)result->status.size());
  for (size_t i = 0; i < result->status.size(); i++) {
    LOG_F(INFO, " * async operation %u", (unsigned)i);
    auto &status = result->status[i];
    LOG_F(INFO, "   * id: %s", status->id.c_str());
    LOG_F(INFO, "   * status: %s", asyncOpState(status->state));
    LOG_F(INFO, "   * time: %lu", (unsigned long)status->creationTime);
    if (!status->txid.empty())
      LOG_F(INFO, "   * txid: %s", status->txid.c_str());
    if (!status->error.empty())
      LOG_F(INFO, "   * error: %s", status->error.c_str());
  }

  return true;
}

bool queryFoundBlocks(PoolRpcCmdContext *context)
{
  if (context->argc != 3) {
    LOG_F(ERROR, "Usage: queryFoundBlocks <heightFrom> <hashFrom> <count>");
    return false;
  }

  auto result = ioQueryFoundBlocks(context->client, xatoi<int64_t>(context->argv[0]), context->argv[1], xatoi<unsigned>(context->argv[2]));
  if (!result)
    return false;
  
  for (size_t i = 0; i < result->blocks.size(); i++) {
    LOG_F(INFO, " * block %u", (unsigned)i);
    
    auto &block = result->blocks[i];
    LOG_F(INFO, "   * height: %llu", block->height);
    LOG_F(INFO, "   * hash: %s", !block->hash.empty() ? block->hash.c_str() : "<empty>");
    LOG_F(INFO, "   * time: %u", (unsigned)block->time);
    LOG_F(INFO, "   * confirmations: %i", (int)block->confirmations);
    LOG_F(INFO, "   * foundBy: %s", !block->foundBy.empty() ? block->foundBy.c_str() : "<empty>");
  }

  return true;
}

bool queryClientInfo(PoolRpcCmdContext *context)
{
  if (context->argc != 1) {
    LOG_F(ERROR, "Usage: queryClientInfo <userId>");
    return false;
  }
  
  auto result = ioQueryClientInfo(context->client, context->argv[0]);
  if (!result)
    return false;
  
  auto &info = result->info;
  LOG_F(INFO, "\nbalance: %.3lf, requested: %.3lf, paid: %.3lf, name: %s, email: %s, minimalPayout: %.3lf",
         (double)info->balance/COIN,
         (double)info->requested/COIN,
         (double)info->paid/COIN,
         !info->name.empty() ? info->name.c_str() : "<empty>",
         !info->email.empty() ? info->email.c_str() : "<empty>",
         (double)(info->minimalPayout)/COIN);

  return true;
}

bool queryPoolBalance(PoolRpcCmdContext *context)
{
  if (context->argc != 2) {
    LOG_F(ERROR, "Usage: queryPoolBalance <timeFrom> <count>");
    return false;
  }

  auto result = ioQueryPoolBalance(context->client, xatoi<int64_t>(context->argv[0]), xatoi<unsigned>(context->argv[1]));
  if (!result)
    return false;
  
  LOG_F(INFO, "\ttime\t\t\tbalance\t\t\timmature\tusers\t\tqueued\t\tnet\n");
  auto recordsNum = result->poolBalances.size();
  for (decltype(recordsNum) i = 0; i < recordsNum; i++) {
    auto &pb = result->poolBalances[i];
    LOG_F(INFO, "\t%u\t\t%.3lf\t\t%.3lf\t\t%.3lf\t\t%.3lf\t\t%.3lf",
           (unsigned)pb->time,
           (double)pb->balance / COIN,
           (double)pb->immature / COIN,
           (double)pb->users / COIN,
           (double)pb->queued / COIN,
           (double)pb->net / COIN);
  }

  return true;
}

bool queryPayouts(PoolRpcCmdContext *context)
{
  if (context->argc != 4) {
    LOG_F(ERROR, "Usage: queryPayouts <userId> <groupingTy> <timeFrom> <count>");
    return false;
  }
  
  
  GroupByType grouping;
  if (strcmp(context->argv[1], "none") == 0) {
    grouping = GroupByType_None;
  } else if (strcmp(context->argv[1], "hour") == 0) {
    grouping = GroupByType_Hour;
  } else if (strcmp(context->argv[1], "day") == 0) {
    grouping = GroupByType_Day;
  } else if (strcmp(context->argv[1], "week") == 0) {
    grouping = GroupByType_Week;
  } else if (strcmp(context->argv[1], "month") == 0) {
    grouping = GroupByType_Month;
  } 

  auto result = ioQueryPayouts(context->client,
                               context->argv[0],
                               grouping,
                               xatoi<int64_t>(context->argv[2]),
                               xatoi<unsigned>(context->argv[3]));
  if (!result)
    return false;

  auto payoutsNum = result->payouts.size();
  for (decltype(payoutsNum) i = 0; i < payoutsNum; i++) {
    auto &record = result->payouts[i];
    LOG_F(INFO, "  %s (%u)    %.3lf    txid: %s",
           record->timeLabel.c_str(),
           (unsigned)record->time,
           (double)record->value / COIN,
           !record->txid.empty() ? record->txid.c_str() : "<no txid>");
  }

  return true;
}

bool queryClientStats(PoolRpcCmdContext *context)
{
  if (context->argc != 1) {
    LOG_F(ERROR, "Usage: queryClientStats <userId>");
    return false;
  }
  
  auto result = ioQueryClientStats(context->client, context->argv[0]);
  if (!result)
    return false;
  
  auto workersNum = result->workers.size();
  for (decltype(workersNum) i = 0; i < workersNum; i++) {
    auto &worker = result->workers[i];
    LOG_F(INFO, "  * %s addr=%s; power=%u; latency: %i; type: %s; units: %u; temp: %i",
           worker->name.c_str(),
           worker->address.c_str(),
           (unsigned)worker->power,
           (int)worker->latency,
           unitType(worker->type),
           (unsigned)worker->units,
           (int)worker->temp);
  }
  
  LOG_F(INFO, "Total:\n  workers: %u\n  cpus:  %u\n  gpus:  %u\n  asics:  %u\n  other:  %u\n  latency: %i\n  power: %u",
         (unsigned)workersNum,
         (unsigned)result->aggregate->cpus,
         (unsigned)result->aggregate->gpus,
         (unsigned)result->aggregate->asics,
         (unsigned)result->aggregate->other,
         (int)result->aggregate->avgerageLatency,
         (unsigned)result->aggregate->power);
  return true;
}

bool queryPoolStats(PoolRpcCmdContext *context)
{
  if (context->argc != 0) {
    LOG_F(ERROR, "Usage: queryPoolStats");
    return false;
  }
  
  auto result = ioQueryPoolStats(context->client);
  if (!result)
    return false;
  
  LOG_F(INFO, "Total:\n  clients: %u\n  workers: %u\n  cpus:  %u\n  gpus:  %u\n  asics:  %u\n  other:  %u\n  latency: %i\n  power: %u",
         (unsigned)result->aggregate->clients,
         (unsigned)result->aggregate->workers,
         (unsigned)result->aggregate->cpus,
         (unsigned)result->aggregate->gpus,
         (unsigned)result->aggregate->asics,
         (unsigned)result->aggregate->other,
         (int)result->aggregate->avgerageLatency,
         (unsigned)result->aggregate->power);
  return true;
}

bool updateClientInfo(PoolRpcCmdContext *context)
{
  if (context->argc != 4) {
    LOG_F(ERROR, "Usage: updateClientInfo <userId> <minimalPayout> <userName> <email>");
    return false;
  }
  
  auto result = ioUpdateClientInfo(context->client, context->argv[0], context->argv[2], context->argv[3], xatoi<int64_t>(context->argv[1]));
  if (!result)
    return false;
  
  LOG_F(INFO, "successfully updated");
  return true;
}

bool resendBrokenTx(PoolRpcCmdContext *context)
{
  if (context->argc != 1) {
    LOG_F(ERROR, "Usage: resendBrokenTx <userId>");
    return false;
  }
  
  auto result = ioResendBrokenTx(context->client, context->argv[0]);
  if (!result)
    return false;
  
  LOG_F(INFO, "successfully called");
  return true;
}

bool moveBalance(PoolRpcCmdContext *context)
{
  if (context->argc != 2) {
    LOG_F(ERROR, "Usage: moveBalance <from> <to>");
    return false;
  }
  
  auto result = ioMoveBalance(context->client, context->argv[0], context->argv[1]);
  if (!result)
    return false;
  
  LOG_F(INFO, result->status == 1 ? "successfully called" : "moveBalance reported error");
  return true;
}

bool manualPayout(PoolRpcCmdContext *context)
{
  if (context->argc != 1) {
    LOG_F(ERROR, "Usage: manualPayout <address>");
    return false;
  }
  
  auto result = ioManualPayout(context->client, context->argv[0]);
  if (!result)
    return false;
  
  LOG_F(INFO, result->status == 1 ? "successfully called" : "manualPayout reported error");
  return true;
}

methodProcTy *getMethodProc(const char *name)
{
  MethodMapElement *element = methodMap;
  while (element->name) {
    if (strcmp(name, element->name) == 0)
      return element->proc;
    element++;
  }
  
  
  return 0;
}

void requestProc(void *arg)
{
  PoolRpcCmdContext *context = (PoolRpcCmdContext*)arg;
  
  if (!context->client->ioWaitForConnection(3000000)) {
    LOG_F(ERROR, "Error: connection error");
    postQuitOperation(context->client->base());
    return;
  }

  methodProcTy *proc = getMethodProc(context->methodName);
  if (!proc) {
    LOG_F(ERROR, "Error: invalid method name: %s", context->methodName);
    postQuitOperation(context->client->base());
    return;
  }
  
  context->result = proc(context);
  postQuitOperation(context->client->base());
}

int main(int argc, char **argv)
{
  if (argc < 3) {
    fprintf(stderr, "usage: %s <url> <command> args...\n", argv[0]);
    return 1;
  }

  loguru::g_preamble = false;
  loguru::g_stderr_verbosity = -1;
  loguru::init(argc, argv);
  loguru::g_stderr_verbosity = 0;

  PoolRpcCmdContext context;
  initializeSocketSubsystem();

  asyncBase *base = createAsyncBase(amOSDefault);

  context.methodName = argv[2];
  context.argc = argc - 3;
  context.argv = argv + 3;
  
  // TODO 127.0.0.1:12200 change to URL from command line
  URI uri;
  if (!uriParse(argv[1], &uri)) {
    LOG_F(ERROR, "<error> Invalid url %s", argv[1]);
    return 1;
  }
  
  context.address.family = AF_INET;
  context.address.ipv4 = uri.ipv4;
  context.address.port = htons(uri.port);
  
  context.client = p2pNode::createClient(base, &context.address, 1, "pool_rpc");
  coroutineTy *proc = coroutineNew(requestProc, &context, 0x10000);  
  coroutineCall(proc);
  asyncLoop(base);
  loguru::g_stderr_verbosity = -1;
  return context.result == true ? 0 : 1;
}
