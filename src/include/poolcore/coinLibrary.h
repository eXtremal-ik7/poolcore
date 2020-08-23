#pragma once

#include "poolcore/poolCore.h"
#include <string.h>

class CCoinLibrary {
public:
  static CCoinInfo get(const char *coinName) {
    CCoinInfo info;
    if (strcmp(coinName, "BTC") == 0) {
      info.Name = "BTC";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {0};
      info.ScriptAddressPrefix = {5};
      info.DefaultRpcPort = 8332;
    } else if (strcmp(coinName, "BTC.testnet") == 0) {
      info.Name = "BTC.testnet";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {111};
      info.ScriptAddressPrefix = {196};
      info.DefaultRpcPort = 18332;
    } else if (strcmp(coinName, "DGB.sha256") == 0) {
      info.Name = "DGB";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {30};
      info.ScriptAddressPrefix = {63};
      info.DefaultRpcPort = 14022;
    } else if (strcmp(coinName, "DGB.sha256.testnet") == 0) {
      info.Name = "DGB.testnet";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {126};
      info.ScriptAddressPrefix = {140};
      info.DefaultRpcPort = 14023;
    } else if (strcmp(coinName, "DOGE") == 0) {
      info.Name = "DOGE";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 3;
      info.PubkeyAddressPrefix = {30};
      info.ScriptAddressPrefix = {22};
      info.DefaultRpcPort = 22555;
    } else if (strcmp(coinName, "DOGE.testnet") == 0) {
      info.Name = "DOGE.testnet";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 3;
      info.PubkeyAddressPrefix = {113};
      info.ScriptAddressPrefix = {196};
      info.DefaultRpcPort = 44555;
    } else if (strcmp(coinName, "LTC") == 0) {
      info.Name = "LTC";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 3;
      info.PubkeyAddressPrefix = {48};
      info.ScriptAddressPrefix = {50};
      info.DefaultRpcPort = 9332;
    } else if (strcmp(coinName, "LTC.testnet") == 0) {
      info.Name = "LTC.testnet";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 3;
      info.PubkeyAddressPrefix = {111};
      info.ScriptAddressPrefix = {58};
      info.DefaultRpcPort = 19332;
    } else if (strcmp(coinName, "XPM") == 0) {
      info.Name = "XPM";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PowerUnitType = CCoinInfo::ECPD;
      info.PowerMultLog10 = -3;
      info.PubkeyAddressPrefix = {23};
      info.DefaultRpcPort = 9912;
    } else if (strcmp(coinName, "XPM.testnet") == 0) {
      info.Name = "XPM.testnet";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PowerUnitType = CCoinInfo::ECPD;
      info.PowerMultLog10 = -3;
      info.PubkeyAddressPrefix = {111};
      info.DefaultRpcPort = 9914;
    } else {
      info.Name.clear();
    }

    return info;
  }
};
