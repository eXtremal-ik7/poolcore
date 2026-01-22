#pragma once

#include "stratumWork.h"
#include "poolcommon/arith_uint256.h"
#include "poolcommon/utils.h"
#include "poolinstances/stratumMsg.h"
#include "poolcommon/jsonSerializer.h"
#include "rapidjson/document.h"
#include <openssl/rand.h>

namespace ETH {

struct BlockSubmitData {
  char Nonce[16+2+1];
  char HeaderHash[64+2+1];
  char MixHash[64+2+1];
};

class Proto {
public:
  using BlockHashTy = BaseBlob<256>;
  using AddressTy = BaseBlob<256>;
  static bool decodeHumanReadableAddress(const std::string&, const std::vector<uint8_t>&, AddressTy&) { return true; }
};

class Stratum {
public:


  static EStratumDecodeStatusTy decodeStratumMessage(CStratumMessage &msg, const char *in, size_t size) {
    rapidjson::Document document;
    document.Parse(in, size);
    if (document.HasParseError()) {
      return EStratumStatusJsonError;
    }

    if (!(document.HasMember("id") && document.HasMember("method") && document.HasMember("params")))
      return EStratumStatusFormatError;

    // Some clients put null to 'params' field
    if (document["params"].IsNull())
      document["params"].SetArray();

    if (!(document["method"].IsString() && document["params"].IsArray()))
      return EStratumStatusFormatError;

    if (document["id"].IsUint64())
      msg.IntegerId = document["id"].GetUint64();
    else if (document["id"].IsString())
      msg.StringId = document["id"].GetString();
    else
      return EStratumStatusFormatError;

    std::string method = document["method"].GetString();
    const rapidjson::Value::Array &params = document["params"].GetArray();
    if (method == "mining.subscribe") {
      msg.Method = ESubscribe;
      if (params.Size() >= 1) {
        if (params[0].IsString())
          msg.Subscribe.minerUserAgent = params[0].GetString();
      }

      if (params.Size() >= 2) {
        if (params[1].IsString())
          msg.Subscribe.StratumVersion = params[1].GetString();
      }

    } else if (method == "mining.authorize" && params.Size() >= 2) {
      msg.Method = EAuthorize;
      if (params[0].IsString() && params[1].IsString()) {
        msg.Authorize.login = params[0].GetString();
        msg.Authorize.password = params[1].GetString();
      } else {
        return EStratumStatusFormatError;
      }
    } else if (method == "mining.extranonce.subscribe") {
      msg.Method = EExtraNonceSubscribe;
    } else if (method == "mining.submit" && params.Size() == 3) {
      msg.Method = ESubmit;
      msg.Submit.WorkerName = params[0].GetString();
      msg.Submit.JobId = params[1].GetString();
      msg.Submit.ETH.Nonce = strtoul(params[2].GetString(), nullptr, 16);
    } else if (method == "eth_submitHashrate") {
      msg.Method = ESubmitHashrate;
    } else {
      return EStratumStatusFormatError;
    }

    return EStratumStatusOk;
  }

  static void miningConfigInitialize(CMiningConfig &miningCfg, rapidjson::Value &instanceCfg) {
    // default values
    miningCfg.FixedExtraNonceSize = 3;

    if (instanceCfg.HasMember("fixedExtraNonceSize") && instanceCfg["fixedExtraNonceSize"].IsUint())
      miningCfg.FixedExtraNonceSize = instanceCfg["fixedExtraNonceSize"].GetUint();
  }

  static void workerConfigInitialize(CWorkerConfig &workerCfg, ThreadConfig &threadCfg) {
    // Set fixed part of extra nonce
    workerCfg.ExtraNonceFixed = threadCfg.ExtraNonceCurrent;

    // Set session names
    uint8_t sessionId[16];
    {
      RAND_bytes(sessionId, sizeof(sessionId));
      workerCfg.NotifySession.resize(sizeof(sessionId)*2);
      bin2hexLowerCase(sessionId, workerCfg.NotifySession.data(), sizeof(sessionId));
    }

    // Update thread config
    threadCfg.ExtraNonceCurrent += threadCfg.ThreadsNum;
  }

  static void workerConfigSetupVersionRolling(CWorkerConfig&, uint32_t) {}

  static void workerConfigOnSubscribe(CWorkerConfig &workerCfg, CMiningConfig &miningCfg, CStratumMessage &msg, xmstream &out, std::string &subscribeInfo) {
    // Response format
    // {"id": 1, "result": [["mining.notify", "ae6812eb4cd7735a302a8a9dd95cf71f", "EthereumStratum/1.0.0"], "080c"],"error": null}

    {
      JSON::Object object(out);
      if (!msg.StringId.empty())
        object.addString("id", msg.StringId);
      else
        object.addInt("id", msg.IntegerId);
      object.addField("result");
      {
        JSON::Array resultValue(out);
        resultValue.addField();
        {
          JSON::Array notifySession(out);
          notifySession.addString("mining.notify");
          notifySession.addString(workerCfg.NotifySession);
          notifySession.addString("EthereumStratum/1.0.0");
        }
        // Unique extra nonce
        resultValue.addString(writeHexBE(workerCfg.ExtraNonceFixed, miningCfg.FixedExtraNonceSize));
      }
      object.addNull("error");
    }

    out.write('\n');
    subscribeInfo = std::to_string(workerCfg.ExtraNonceFixed);
  }

  class Work : public StratumSingleWork {
  public:
    Work(int64_t stratumWorkId, uint64_t uniqueWorkId, PoolBackend *backend, size_t backendIdx, const CMiningConfig &miningCfg, const std::vector<uint8_t>&, const std::string&) :
      StratumSingleWork(stratumWorkId, uniqueWorkId, backend, backendIdx, miningCfg) {
      Initialized_ = true;
    }

    virtual Proto::BlockHashTy shareHash() override {
      BaseBlob<256> hash;
      memcpy(hash.begin(), FinalHash_.begin(), 32);
      return hash;
    }

    virtual std::string blockHash(size_t) override {
      uint256 hash(MixHash_);
      std::reverse(hash.begin(), hash.end());
      return hash.ToString();
    }

    virtual double expectedWork(size_t) override {
      // TODO: implement
      return 0.0;
    }

    virtual bool ready() override {
      return true;
    }

    virtual void buildBlock(size_t, xmstream &blockHexData) override;

    virtual void mutate() override {}

    virtual CCheckStatus checkConsensus(size_t) override;

    virtual bool hasRtt(size_t) override { return false; }

    virtual void buildNotifyMessage(bool resetPreviousWork) override;

    virtual bool loadFromTemplate(CBlockTemplate &blockTemplate, std::string &error) override;

    virtual bool prepareForSubmit(const CWorkerConfig &workerCfg, const CStratumMessage&msg) override;

    virtual double getAbstractProfitValue(size_t, double, double) override {
      // TODO: calculate real profit value
      return 0.00000001;
    }

  private:
    std::string HeaderHashHex_;
    std::string SeedHashHex_;
    uint256 HeaderHash_;
    arith_uint256 Target_;
    uint64_t Nonce_ = 0;
    arith_uint256 FinalHash_;
    uint256 MixHash_;
    intrusive_ptr<EthashDagWrapper> DagFile_;
  };

  static constexpr bool MergedMiningSupport = false;

  static Work *newPrimaryWork(int64_t stratumId,
      PoolBackend *backend,
      size_t backendIdx,
      const CMiningConfig &miningCfg,
      const std::vector<uint8_t> &miningAddress,
      const std::string &coinbaseMessage,
      CBlockTemplate &blockTemplate,
      std::string &error) {
    std::unique_ptr<Work> work(new Work(stratumId,
                                        blockTemplate.UniqueWorkId,
                                        backend,
                                        backendIdx,
                                        miningCfg,
                                        miningAddress,
                                        coinbaseMessage));
    return work->loadFromTemplate(blockTemplate, error) ? work.release() : nullptr;
  }
  static StratumSingleWork *newSecondaryWork(int64_t, PoolBackend*, size_t, const CMiningConfig&, const std::vector<uint8_t>&, const std::string&, CBlockTemplate&, const std::string&) { return nullptr; }
  static StratumMergedWork *newMergedWork(int64_t, StratumSingleWork*, std::vector<StratumSingleWork*>&, const CMiningConfig&, std::string&) { return nullptr; }


  static void buildSendTargetMessage(xmstream &stream, double difficulty) {
    JSON::Object object(stream);
    object.addString("method", "mining.set_difficulty");
    object.addNull("id");
    object.addField("params");
    {
      JSON::Array params(stream);
      params.addDouble(difficulty);
    }
  }

  using SecondWork = StratumSingleWorkEmpty;
  using MergedWork = StratumMergedWorkEmpty;
};

struct X {
  using Proto = ETH::Proto;
  using Stratum = ETH::Stratum;
};
}
