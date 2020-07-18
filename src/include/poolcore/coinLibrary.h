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
      info.PubkeyAddressPrefix = {0};
      info.ScriptAddressPrefix = {5};
      info.DefaultRpcPort = 8332;
    } else if (strcmp(coinName, "BTC.testnet") == 0) {
      info.Name = "BTC.testnet";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PubkeyAddressPrefix = {111};
      info.ScriptAddressPrefix = {196};
      info.DefaultRpcPort = 18332;
    } else if (strcmp(coinName, "LTC") == 0) {
      info.Name = "LTC";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PubkeyAddressPrefix = {48};
      info.ScriptAddressPrefix = {50};
      info.DefaultRpcPort = 9332;
    } else if (strcmp(coinName, "LTC.testnet") == 0) {
      info.Name = "LTC.testnet";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PubkeyAddressPrefix = {111};
      info.ScriptAddressPrefix = {58};
      info.DefaultRpcPort = 19332;
    } else if (strcmp(coinName, "XPM") == 0) {
      info.Name = "XPM";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PubkeyAddressPrefix = {23};
      info.DefaultRpcPort = 9912;
    } else if (strcmp(coinName, "XPM.testnet") == 0) {
      info.Name = "XPM.testnet";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PubkeyAddressPrefix = {111};
      info.DefaultRpcPort = 9914;
    } else {
      info.Name.clear();
    }

    return info;
  }
};
