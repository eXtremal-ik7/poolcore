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
    } else if (strcmp(coinName, "XPM") == 0) {
      info.Name = "XPM";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PubkeyAddressPrefix = {23};
    } else {
      info.Name.clear();
    }

    return info;
  }
};
