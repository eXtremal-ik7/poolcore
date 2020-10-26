#pragma once

#include "poolcore/poolCore.h"
#include <string.h>

class CCoinLibrary {
public:
  static CCoinInfo get(const char *coinName) {
    CCoinInfo info;
    // PoW algorithms
    if (strcmp(coinName, "sha256") == 0) {
      info.Name = "sha256";
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
    } else if (strcmp(coinName, "scrypt") == 0) {
      info.Name = "scrypt";
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 3;
    } else if (strcmp(coinName, "primecoin") == 0) {
      info.Name = "primecoin";
      info.PowerUnitType = CCoinInfo::ECPD;
      info.PowerMultLog10 = 1;
    } else if (strcmp(coinName, "BTC") == 0) {
      info.Name = "BTC";
      info.FullName = "Bitcoin";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {0};
      info.ScriptAddressPrefix = {5};
      info.Bech32Prefix = "bc";
      info.DefaultRpcPort = 8332;
      info.CoinGeckoName = "bitcoin";
      info.ProfitSwitchDefaultCoeff = 1.0;
      info.Algorithm = "sha256";
    } else if (strcmp(coinName, "BTC.testnet") == 0) {
      info.Name = "BTC.testnet";
      info.FullName = "Bitcoin";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {111};
      info.ScriptAddressPrefix = {196};
      info.Bech32Prefix = "tb";
      info.DefaultRpcPort = 18332;
      info.Algorithm = "sha256";
    } else if (strcmp(coinName, "BTC.regtest") == 0) {
      info.Name = "BTC.regtest";
      info.FullName = "Bitcoin";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {111};
      info.ScriptAddressPrefix = {196};
      info.Bech32Prefix = "bcrt";
      info.DefaultRpcPort = 18443;
      info.Algorithm = "sha256";
    } else if (strcmp(coinName, "BCH") == 0) {
      info.Name = "BCH";
      info.FullName = "Bitcoin Cash";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {0};
      info.ScriptAddressPrefix = {5};
      info.DefaultRpcPort = 8332;
      info.CoinGeckoName = "bitcoin-cash";
      info.Algorithm = "sha256";
    } else if (strcmp(coinName, "BCH.testnet") == 0) {
      info.FullName = "Bitcoin Cash";
      info.Name = "BCH.testnet";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {111};
      info.ScriptAddressPrefix = {196};
      info.DefaultRpcPort = 18332;
      info.Algorithm = "sha256";
    } else if (strcmp(coinName, "BCHN") == 0) {
      info.Name = "BCHN";
      info.FullName = "Bitcoin Cash Node";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {0};
      info.ScriptAddressPrefix = {5};
      info.DefaultRpcPort = 8332;
      info.CoinGeckoName = "bitcoin-cash";
      info.Algorithm = "sha256";
    } else if (strcmp(coinName, "BSV") == 0) {
      info.Name = "BSV";
      info.FullName = "Bitcoin SV";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {0};
      info.ScriptAddressPrefix = {5};
      info.DefaultRpcPort = 8332;
      info.CoinGeckoName = "bitcoin-cash-sv";
      info.Algorithm = "sha256";
    } else if (strcmp(coinName, "BSV.testnet") == 0) {
      info.Name = "BSV.testnet";
      info.FullName = "Bitcoin SV";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {111};
      info.ScriptAddressPrefix = {196};
      info.DefaultRpcPort = 18332;
      info.Algorithm = "sha256";
    } else if (strcmp(coinName, "DGB.sha256") == 0) {
      info.Name = "DGB.sha256";
      info.FullName = "Digibyte(sha256)";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {30};
      info.ScriptAddressPrefix = {63};
      info.Bech32Prefix = "dgb";
      info.DefaultRpcPort = 14022;
      info.CoinGeckoName = "digibyte";
      info.ProfitSwitchDefaultCoeff = 0.7;
      info.MinimalConfirmationsNumber = 32;
      info.Algorithm = "sha256";
    } else if (strcmp(coinName, "DGB.sha256.testnet") == 0) {
      info.Name = "DGB.sha256.testnet";
      info.FullName = "Digibyte";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {126};
      info.ScriptAddressPrefix = {140};
      info.Bech32Prefix = "dgbt";
      info.DefaultRpcPort = 14023;
      info.MinimalConfirmationsNumber = 32;
      info.Algorithm = "sha256";
    } else if (strcmp(coinName, "DGB.scrypt") == 0) {
      info.Name = "DGB.scrypt";
      info.FullName = "Digibyte";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 3;
      info.PubkeyAddressPrefix = {30};
      info.ScriptAddressPrefix = {63};
      info.Bech32Prefix = "dgb";
      info.DefaultRpcPort = 14022;
      info.CoinGeckoName = "digibyte";
      info.ProfitSwitchDefaultCoeff = 0.7;
      info.MinimalConfirmationsNumber = 32;
      info.Algorithm = "scrypt";
    } else if (strcmp(coinName, "DGB.scrypt.testnet") == 0) {
      info.Name = "DGB.scrypt.testnet";
      info.FullName = "Digibyte";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 3;
      info.PubkeyAddressPrefix = {126};
      info.ScriptAddressPrefix = {140};
      info.Bech32Prefix = "dgbt";
      info.DefaultRpcPort = 14023;
      info.MinimalConfirmationsNumber = 32;
      info.Algorithm = "scrypt";
    } else if (strcmp(coinName, "DOGE") == 0) {
      info.Name = "DOGE";
      info.FullName = "Dogecoin";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 3;
      info.PubkeyAddressPrefix = {30};
      info.ScriptAddressPrefix = {22};
      info.DefaultRpcPort = 22555;
      info.CoinGeckoName = "dogecoin";
      info.ProfitSwitchDefaultCoeff = 1.0;
      info.MinimalConfirmationsNumber = 12;
      info.Algorithm = "scrypt";
    } else if (strcmp(coinName, "DOGE.testnet") == 0) {
      info.Name = "DOGE.testnet";
      info.FullName = "Dogecoin";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 3;
      info.PubkeyAddressPrefix = {113};
      info.ScriptAddressPrefix = {196};
      info.DefaultRpcPort = 44555;
      info.MinimalConfirmationsNumber = 12;
      info.Algorithm = "scrypt";
    } else if (strcmp(coinName, "FCH") == 0) {
      info.Name = "FCH";
      info.FullName = "FreeCash";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {35};
      info.ScriptAddressPrefix = {5};
      info.DefaultRpcPort = 8332;
      info.ProfitSwitchDefaultCoeff = 0.4;
      info.CoinGeckoName = "freecash";
      info.Algorithm = "sha256";
    } else if (strcmp(coinName, "LCC") == 0) {
      info.Name = "LCC";
      info.FullName = "LitecoinCash";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 6;
      info.PubkeyAddressPrefix = {28};
      info.ScriptAddressPrefix = {5};
      info.DefaultRpcPort = 62457;
      info.ProfitSwitchDefaultCoeff = 0.1;
      info.CoinGeckoName = "litecoin-cash";
      info.Algorithm = "sha256";
    } else if (strcmp(coinName, "LTC") == 0) {
      info.Name = "LTC";
      info.FullName = "Litecoin";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 3;
      info.PubkeyAddressPrefix = {48};
      info.ScriptAddressPrefix = {50};
      info.Bech32Prefix = "ltc";
      info.DefaultRpcPort = 9332;
      info.CoinGeckoName = "litecoin";
      info.ProfitSwitchDefaultCoeff = 1.0;
      info.MinimalConfirmationsNumber = 12;
      info.Algorithm = "scrypt";
    } else if (strcmp(coinName, "LTC.testnet") == 0) {
      info.Name = "LTC.testnet";
      info.FullName = "Litecoin";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH | CCoinInfo::EPS2H | CCoinInfo::EBech32);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = true;
      info.PowerUnitType = CCoinInfo::EHash;
      info.PowerMultLog10 = 3;
      info.PubkeyAddressPrefix = {111};
      info.ScriptAddressPrefix = {58};
      info.Bech32Prefix = "tltc";
      info.DefaultRpcPort = 19332;
      info.MinimalConfirmationsNumber = 12;
      info.Algorithm = "scrypt";
    } else if (strcmp(coinName, "XPM") == 0) {
      info.Name = "XPM";
      info.FullName = "Primecoin";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PowerUnitType = CCoinInfo::ECPD;
      info.PowerMultLog10 = -3;
      info.PubkeyAddressPrefix = {23};
      info.DefaultRpcPort = 9912;
      info.CoinGeckoName = "primecoin";
      info.ProfitSwitchDefaultCoeff = 1.0;
      info.MinimalConfirmationsNumber = 12;
      info.Algorithm = "primecoin";
    } else if (strcmp(coinName, "XPM.testnet") == 0) {
      info.Name = "XPM.testnet";
      info.FullName = "Primecoin";
      info.PayoutAddressType = static_cast<CCoinInfo::EAddressType>(CCoinInfo::EP2PKH);
      info.RationalPartSize = 100000000;
      info.SegwitEnabled = false;
      info.PowerUnitType = CCoinInfo::ECPD;
      info.PowerMultLog10 = -3;
      info.PubkeyAddressPrefix = {111};
      info.DefaultRpcPort = 9914;
      info.MinimalConfirmationsNumber = 12;
      info.Algorithm = "primecoin";
    } else {
      info.Name.clear();
    }

    return info;
  }
};
