#pragma once

#include "stratumWork.h"
#include "poolcommon/uint256.h"
#include "poolinstances/stratumMsg.h"
#include "poolcommon/jsonSerializer.h"
#include "rapidjson/document.h"
#include <openssl/rand.h>

namespace ETH {

class Proto {
public:
  using BlockHashTy = uint256;
  using AddressTy = uint256;
  static bool decodeHumanReadableAddress(const std::string&, const std::vector<uint8_t>&, AddressTy&) { return true; }
};

class Stratum {
public:
  struct StratumMiningSubscribe {
    std::string minerUserAgent;
    std::string StratumVersion;
  };

  struct StratumMessage {
    int64_t IntegerId;
    std::string StringId;
    EStratumMethodTy Method;

    StratumMiningSubscribe Subscribe;
    StratumAuthorize Authorize;
    StratumMiningConfigure MiningConfigure;
    StratumSubmit Submit;

    EStratumDecodeStatusTy decodeStratumMessage(const char *in, size_t size);

    void addId(JSON::Object &object) {
      if (!StringId.empty())
        object.addString("id", StringId);
      else
        object.addInt("id", IntegerId);
    }
  };

  struct MiningConfig {
    unsigned FixedExtraNonceSize = 3;

    void initialize(rapidjson::Value &instanceCfg) {
      if (instanceCfg.HasMember("fixedExtraNonceSize") && instanceCfg["fixedExtraNonceSize"].IsUint())
        FixedExtraNonceSize = instanceCfg["fixedExtraNonceSize"].GetUint();
    }
  };

  struct WorkerConfig {
    uint64_t ExtraNonceFixed;
    std::string NotifySession;

    static inline void addId(JSON::Object &object, StratumMessage &msg) {
      if (!msg.StringId.empty())
        object.addString("id", msg.StringId);
      else
        object.addInt("id", msg.IntegerId);
    }

    void initialize(ThreadConfig &threadCfg) {
      // Set fixed part of extra nonce
      ExtraNonceFixed = threadCfg.ExtraNonceCurrent;

      // Set session names
      uint8_t sessionId[16];
      {
        RAND_bytes(sessionId, sizeof(sessionId));
        NotifySession.resize(sizeof(sessionId)*2);
        bin2hexLowerCase(sessionId, NotifySession.data(), sizeof(sessionId));
      }

      // Update thread config
      threadCfg.ExtraNonceCurrent += threadCfg.ThreadsNum;
    }

    void onSubscribe(MiningConfig &miningCfg, StratumMessage &msg, xmstream &out, std::string &subscribeInfo);

    // TODO: remove
    void setupVersionRolling(uint32_t) {}
  };

  using CSingleWork = StratumSingleWork<Proto::BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>;

  class Work : public CSingleWork {
  public:
    Work(int64_t stratumWorkId, uint64_t uniqueWorkId, PoolBackend *backend, size_t backendIdx, const MiningConfig &miningCfg, const std::vector<uint8_t>&, const std::string&) :
      CSingleWork(stratumWorkId, uniqueWorkId, backend, backendIdx, miningCfg) {
      Initialized_ = true;
    }

    virtual Proto::BlockHashTy shareHash() override {
      // TODO: implement
      return uint256();
    }

    virtual std::string blockHash(size_t) override {
      // TODO: implement
      return uint256().ToString();
    }

    virtual double expectedWork(size_t) override {
      // TODO: implement
      return 0.0;
    }

    virtual bool ready() override {
      return true;
    }

    virtual void buildBlock(size_t, xmstream &blockHexData) override {
      // TODO: implement
    }

    virtual void mutate() override {}

    virtual bool checkConsensus(size_t, double *shareDiff) override {
      // TODO: implement
      return false;
    }

    virtual void buildNotifyMessage(bool resetPreviousWork) override;

    virtual bool loadFromTemplate(rapidjson::Value &document, const std::string &ticker, std::string &error) override;

    virtual bool prepareForSubmit(const WorkerConfig&, const StratumMessage&) override {
      // TODO: implement
      return false;
    }

    virtual double getAbstractProfitValue(size_t, double, double) override {
      // TODO: calculate real profit value
      return 0.00000001;
    }

  private:
    std::string SeedHash_;
    std::string HeaderHash_;
  };

  static constexpr bool MergedMiningSupport = false;
  static bool isMainBackend(const std::string&) { return true; }
  static bool keepOldWorkForBackend(const std::string&) { return false; }

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

  using SecondWork = StratumSingleWorkEmpty<Proto::BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>;
  using MergedWork = StratumMergedWorkEmpty<Proto::BlockHashTy, MiningConfig, WorkerConfig, StratumMessage>;
};

struct X {
  using Proto = ETH::Proto;
  using Stratum = ETH::Stratum;
};
}
