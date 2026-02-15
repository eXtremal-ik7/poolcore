#include <stdio.h>
#include "poolcommon/utils.h"
#include "poolcore/backendData.h"
#include "poolcore/bitcoinRPCClient.h"
#include "poolcore/coinLibrary.h"
#include "poolcore/ethereumRPCClient.h"
#include "poolcore/plugin.h"
#include "loguru.hpp"
#include <inttypes.h>
#include <getopt.h>

CPluginContext gPluginContext;

enum CmdLineOptsTy {
  clOptHelp = 1,
  clOptNode,
  clOptCoin,
  clOptAddress,
  clOptUser,
  clOptPassword,
  clOptWallet,
  clOptMethod,
  clOptMiningAddresses,
  clOptMiningPrivateKeys
};

static option cmdLineOpts[] = {
  {"help", no_argument, nullptr, clOptHelp},
  {"node", required_argument, nullptr, clOptNode},
  {"coin", required_argument, nullptr, clOptCoin},
  {"address", required_argument, nullptr, clOptAddress},
  {"user", required_argument, nullptr, clOptUser},
  {"password", required_argument, nullptr, clOptPassword},
  {"wallet", required_argument, nullptr, clOptWallet},
  {"method", required_argument, nullptr, clOptMethod},
  {"mining-addresses", required_argument, nullptr, clOptMiningAddresses},
  {"private-keys", required_argument, nullptr, clOptMiningPrivateKeys},
  {nullptr, 0, nullptr, 0}
};

struct CContext {
  asyncBase *Base;
  std::unique_ptr<CNetworkClient> Client;
  CCoinInfo CoinInfo;
  char **Argv;
  size_t ArgsNum;
};

void getBalanceCoro(CContext *context)
{
  CNetworkClient::GetBalanceResult result;
  if (context->Client->ioGetBalance(context->Base, result)) {
    LOG_F(INFO, "balance: %s immature: %s\n", FormatMoney(result.Balance, context->CoinInfo.FractionalPartSize).c_str(), FormatMoney(result.Immatured, context->CoinInfo.FractionalPartSize).c_str());
  } else {
    LOG_F(ERROR, "ioGetBalance failed");
  }
}

void buildTransactionCoro(CContext *context)
{
  if (context->ArgsNum != 3) {
    LOG_F(INFO, "Usage: buildTransaction <address> <change_address> <amount>");
    return;
  }

  const char *destinationAddress = context->Argv[0];
  const char *changeAddress = context->Argv[1];
  const char *amount = context->Argv[2];

  if (!context->CoinInfo.checkAddress(destinationAddress, context->CoinInfo.PayoutAddressType)) {
    LOG_F(INFO, "Invalid %s address: %s", context->CoinInfo.Name.c_str(), destinationAddress);
    return;
  }

  UInt<384> value;
  if (!parseMoneyValue(amount, context->CoinInfo.FractionalPartSize, &value)) {
    LOG_F(INFO, "Invalid amount: %s %s", amount, context->CoinInfo.Name.c_str());
    return;
  }

  CNetworkClient::BuildTransactionResult transaction;
  CNetworkClient::EOperationStatus status =
    context->Client->ioBuildTransaction(context->Base, destinationAddress, changeAddress, value, transaction);
  if (status == CNetworkClient::EStatusOk) {
    // Nothing to do
  } else if (status == CNetworkClient::EStatusInsufficientFunds) {
    LOG_F(INFO, "No money left to pay");
    return;
  } else {
    LOG_F(ERROR, "Payment %s to %s failed with error \"%s\"", FormatMoney(value, context->CoinInfo.FractionalPartSize).c_str(), destinationAddress, transaction.Error.c_str());
    return;
  }

  LOG_F(INFO, "txData: %s", transaction.TxData.c_str());
  LOG_F(INFO, "txId: %s", transaction.TxId.c_str());
  LOG_F(INFO, "real value: %s", FormatMoney(transaction.Value, context->CoinInfo.FractionalPartSize).c_str());
  LOG_F(INFO, "fee: %s", FormatMoney(transaction.Fee, context->CoinInfo.FractionalPartSize).c_str());
}

void sendTransactionCoro(CContext *context)
{
  if (context->ArgsNum != 2) {
    LOG_F(INFO, "Usage: sendTransaction <txdata> <txid>");
    return;
  }

  const char *txData = context->Argv[0];
  const char *txId = context->Argv[1];
  std::string error;
  CNetworkClient::EOperationStatus status =
    context->Client->ioSendTransaction(context->Base, txData, txId, error);
  if (status == CNetworkClient::EStatusOk) {
    // Nothing to do
  } else if (status == CNetworkClient::EStatusVerifyRejected) {
    LOG_F(ERROR, "Transaction %s rejected", txData);
    return;
  } else {
    LOG_F(WARNING, "Sending transaction %s error \"%s\", will try send later...", txData, error.c_str());
    return;
  }

  LOG_F(INFO, "sending ok");
}

void getTxConfirmationsCoro(CContext *context)
{
  if (context->ArgsNum != 1) {
    LOG_F(INFO, "Usage: getTxConfirmations <txid>");
    return;
  }

  const char *txId = context->Argv[0];
  std::string error;
  int64_t confirmations = 0;
  UInt<384> txFee = UInt<384>::zero();
  CNetworkClient::EOperationStatus status = context->Client->ioGetTxConfirmations(context->Base, txId, &confirmations, &txFee, error);
  if (status == CNetworkClient::EStatusOk) {
    // Nothing to do
  } else if (status == CNetworkClient::EStatusInvalidAddressOrKey) {
    LOG_F(ERROR, "Transaction %s not included in block, will try resend", txId);
    return;
  } else {
    LOG_F(WARNING, "Transaction %s checkong error \"%s\", will try later...", txId, error.c_str());
    return;
  }

  LOG_F(INFO, "Confirmations: %" PRIi64 "", confirmations);
  LOG_F(INFO, "Fee: %s\n", FormatMoney(txFee, context->CoinInfo.FractionalPartSize).c_str());
}

void getBlockConfirmationCoro(CContext *context)
{
  if (context->ArgsNum != 2) {
    LOG_F(INFO, "Usage: getBlockConfirmation <hash> <height>");
    return;
  }

  const char *blockHash = context->Argv[0];
  const char *blockHeight = context->Argv[1];

  std::vector<CNetworkClient::GetBlockConfirmationsQuery> query;
  CNetworkClient::GetBlockConfirmationsQuery &queryElement = query.emplace_back();
  queryElement.Hash = blockHash;
  queryElement.Height = xatoi<uint64_t>(blockHeight);
  if (context->Client->ioGetBlockConfirmations(context->Base, 0, query)) {
    LOG_F(INFO, "confirmations: %" PRId64 "", queryElement.Confirmations);
  } else {
    LOG_F(ERROR, "can't get confirmations for %s", blockHash);
  }
}

void getBlockExtraInfoCoro(CContext *context)
{
  if (context->ArgsNum != 2) {
    LOG_F(INFO, "Usage: getBlockExtraInfo <hash> <height>");
    return;
  }

  const char *blockHash = context->Argv[0];
  const char *blockHeight = context->Argv[1];

  std::vector<CNetworkClient::GetBlockExtraInfoQuery> query;
  auto &queryElement = query.emplace_back();
  queryElement.Hash = blockHash;
  queryElement.Height = xatoi<uint64_t>(blockHeight);
  queryElement.TxFee = UInt<384>::zero();
  queryElement.BlockReward = UInt<384>::zero();
  if (context->Client->ioGetBlockExtraInfo(context->Base, 0, query)) {
    LOG_F(INFO, "confirmations: %" PRId64 "", queryElement.Confirmations);
    LOG_F(INFO, "public hash: %s", queryElement.PublicHash.c_str());
    LOG_F(INFO, "tx fee: %s", FormatMoney(queryElement.TxFee, context->CoinInfo.FractionalPartSize).c_str());
    LOG_F(INFO, "block reward: %s", FormatMoney(queryElement.BlockReward, context->CoinInfo.FractionalPartSize).c_str());
  } else {
    LOG_F(ERROR, "can't get confirmations for %s", blockHash);
  }
}

void listUnspentCoro(CContext *context)
{
  if (context->ArgsNum != 0) {
    LOG_F(INFO, "Usage: listUnspent");
    return;
  }

  CNetworkClient::ListUnspentResult unspent;
  CNetworkClient::EOperationStatus status = context->Client->ioListUnspent(context->Base, unspent);
  if (status == CNetworkClient::EStatusOk) {
    LOG_F(INFO, "Unspent outputs:");
    for (const auto &output: unspent.Outs) {
      LOG_F(INFO, "%s: %s; isCoinbase: %s", output.Address.c_str(), FormatMoney(output.Amount, context->CoinInfo.FractionalPartSize).c_str(), output.IsCoinbase ? "yes" : "no");
    }
  } else {
    LOG_F(ERROR, "listUnspent error %u", status);
  }
}

void zsendManyCoro(CContext *context)
{
  if (context->ArgsNum != 6) {
    LOG_F(INFO, "Usage: zsendMany <source> <destination> <amount> <memo> <minconf> <fee>");
    return;
  }

  const char *source = context->Argv[0];
  const char *destination = context->Argv[1];

  UInt<384> amount;
  if (!parseMoneyValue(context->Argv[2], context->CoinInfo.FractionalPartSize, &amount)) {
    LOG_F(ERROR, "Can't parse amount value %s", context->Argv[2]);
    return;
  }

  const char *memo = context->Argv[3];
  uint64_t minConf = xatoi<uint64_t>(context->Argv[4]);

  UInt<384> fee;
  if (!parseMoneyValue(context->Argv[5], context->CoinInfo.FractionalPartSize, &fee)) {
    LOG_F(ERROR, "Can't parse fee value %s", context->Argv[5]);
    return;
  }

  CNetworkClient::ZSendMoneyResult result;
  CNetworkClient::EOperationStatus status = context->Client->ioZSendMany(context->Base, source, destination, amount, memo, minConf, fee, result);
  if (status == CNetworkClient::EStatusOk) {
    LOG_F(INFO, "AsyncOp ID: %s", result.AsyncOperationId.c_str());
  } else {
    LOG_F(ERROR, "zsendMany error: %s", result.Error.c_str());
  }
}

void zgetBalanceCoro(CContext *context)
{
  if (context->ArgsNum != 1) {
    LOG_F(INFO, "Usage: zgetBalance <address>");
    return;
  }

  UInt<384> balance = UInt<384>::zero();
  CNetworkClient::EOperationStatus status = context->Client->ioZGetBalance(context->Base, context->Argv[0], &balance);
  if (status == CNetworkClient::EStatusOk) {
    LOG_F(INFO, "balance: %s", FormatMoney(balance, context->CoinInfo.FractionalPartSize).c_str());
  } else {
    LOG_F(ERROR, "zgetBalance error");
  }
}


void printHelpMessage()
{
  printf("noderpc usage:\n");
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

  std::string type;
  std::string coin;
  const char *address = nullptr;
  const char *user = "";
  const char *password = "";
  const char *wallet = "";
  std::string method;
  std::vector<std::string> miningAddresses;
  std::vector<std::string> privateKeys;

  // Parsing command line
  int res;
  int index = 0;
  while ((res = getopt_long(argc, argv, "", cmdLineOpts, &index)) != -1) {
    switch (res) {
      case clOptHelp :
        printHelpMessage();
        return 0;
      case clOptNode:
        type = optarg;
        break;
      case clOptCoin:
        coin = optarg;
        break;
      case clOptAddress:
        address = optarg;
        break;
      case clOptUser:
        user = optarg;
        break;
      case clOptPassword:
        password = optarg;
        break;
      case clOptWallet:
        wallet = optarg;
        break;
      case clOptMethod:
        method = optarg;
        break;
      case clOptMiningAddresses: {
        const char *p = optarg;
        while (p) {
          const char *commaPtr = strchr(p, ',');
          miningAddresses.emplace_back(p, commaPtr ? commaPtr-p : strlen(p));
          p = commaPtr ? commaPtr+1 : nullptr;
        }
        break;
      }
      case clOptMiningPrivateKeys: {
        const char *p = optarg;
        while (p) {
          const char *commaPtr = strchr(p, ',');
          privateKeys.emplace_back(p, commaPtr ? commaPtr-p : strlen(p));
          p = commaPtr ? commaPtr+1 : nullptr;
        }
      break;
    }
      case ':' :
        fprintf(stderr, "Error: option %s missing argument\n", cmdLineOpts[index].name);
        break;
      case '?' :
        exit(1);
      default :
        break;
    }
  }

  if (type.empty() || coin.empty() || !address || method.empty()) {
    fprintf(stderr, "Error: you must specify --node, --coin, --address, --method\n");
    exit(1);
  }

  if (!privateKeys.empty() && privateKeys.size() != miningAddresses.size()) {
    fprintf(stderr, "Error: private keys amount must be equal to mining addresses amount\n");
    exit(1);
  }

  CContext context;
  initializeSocketSubsystem();
  context.Base = createAsyncBase(amOSDefault);
  context.Argv = argv + optind;
  context.ArgsNum = argc - optind;

  context.CoinInfo = CCoinLibrary::get(coin.c_str());
  if (context.CoinInfo.Name.empty()) {
    // load coin info from extra directory
    bool foundInExtra = false;
    for (const auto &proc: gPluginContext.AddExtraCoinProcs) {
      if (proc(coin.c_str(), context.CoinInfo)) {
        foundInExtra = true;
        break;
      }
    }

    if (!foundInExtra) {
      LOG_F(ERROR, "Unknown coin: %s", coin.c_str());
      return 1;
    }
  }

  // Create node
  CNodeConfig nodeConfig;
  PoolBackendConfig config;

  nodeConfig.Address = address;
  nodeConfig.Login = user;
  nodeConfig.Password = password;
  nodeConfig.Wallet = wallet;

  for (size_t i = 0, ie = miningAddresses.size(); i != ie; ++i)
    config.MiningAddresses.add(CMiningAddress(miningAddresses[i], !privateKeys.empty() ? privateKeys[i] : ""), 1);

  if (type == "bitcoinrpc") {
    if (!user || !password) {
      fprintf(stderr, "Error: you must specify --user and --password\n");
      exit(1);
    }
    context.Client.reset(new CBitcoinRpcClient(context.Base, 1, context.CoinInfo, address, user, password, wallet, true));
  } else if (type == "ethereumrpc") {
    if ((method == "getBalance" || method == "buildTransaction") &&
        miningAddresses.size() != 1) {
      fprintf(stderr, "Error: you must specify single mining address\n");
      exit(1);
    }

    context.Client.reset(new CEthereumRpcClient(context.Base, 1, context.CoinInfo, address, config));
  } else {
    // lookup client type in extras
    for (const auto &proc: gPluginContext.AddRpcClientForTerminalProcs) {
      context.Client.reset(proc(type, context.Base, 1, context.CoinInfo, nodeConfig, config, method, miningAddresses, privateKeys));
      if (context.Client)
        break;
    }

    if (!context.Client) {
      LOG_F(ERROR, "Unknown node type: %s", type.c_str());
      return 1;
    }
  }

  if (method == "getBalance") {
    coroutineCall(coroutineNew([](void *arg) {
      getBalanceCoro(static_cast<CContext*>(arg));
      postQuitOperation(static_cast<CContext*>(arg)->Base);
    }, &context, 0x10000));
  } else if (method == "buildTransaction") {
    coroutineCall(coroutineNew([](void *arg) {
      buildTransactionCoro(static_cast<CContext*>(arg));
      postQuitOperation(static_cast<CContext*>(arg)->Base);
    }, &context, 0x10000));
  } else if (method == "sendTransaction") {
    coroutineCall(coroutineNew([](void *arg) {
      sendTransactionCoro(static_cast<CContext*>(arg));
      postQuitOperation(static_cast<CContext*>(arg)->Base);
    }, &context, 0x10000));
  } else if (method == "getTxConfirmations") {
    coroutineCall(coroutineNew([](void *arg) {
      getTxConfirmationsCoro(static_cast<CContext*>(arg));
      postQuitOperation(static_cast<CContext*>(arg)->Base);
    }, &context, 0x10000));
  } else if (method == "getBlockConfirmation") {
    coroutineCall(coroutineNew([](void *arg) {
      getBlockConfirmationCoro(static_cast<CContext*>(arg));
      postQuitOperation(static_cast<CContext*>(arg)->Base);
    }, &context, 0x10000));
  } else if (method == "getBlockExtraInfo") {
    coroutineCall(coroutineNew([](void *arg) {
      getBlockExtraInfoCoro(static_cast<CContext*>(arg));
      postQuitOperation(static_cast<CContext*>(arg)->Base);
    }, &context, 0x10000));
  } else if (method == "listUnspent") {
    coroutineCall(coroutineNew([](void *arg) {
      listUnspentCoro(static_cast<CContext*>(arg));
      postQuitOperation(static_cast<CContext*>(arg)->Base);
    }, &context, 0x10000));
  } else if (method == "zsendMany") {
    coroutineCall(coroutineNew([](void *arg) {
      zsendManyCoro(static_cast<CContext*>(arg));
      postQuitOperation(static_cast<CContext*>(arg)->Base);
    }, &context, 0x10000));
  } else if (method == "zgetBalance") {
    coroutineCall(coroutineNew([](void *arg) {
      zgetBalanceCoro(static_cast<CContext*>(arg));
      postQuitOperation(static_cast<CContext*>(arg)->Base);
    }, &context, 0x10000));
  } else {
    LOG_F(ERROR, "Unknown method: %s", method.c_str());
    return 1;
  }

  asyncLoop(context.Base);
  return 0;
}
