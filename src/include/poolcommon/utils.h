#pragma once

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <stdarg.h>
#include <string>

static constexpr int64_t COIN = 100000000;

std::string real_strprintf(const std::string &format, int dummy, ...);
#define strprintf(format, ...) real_strprintf(format, 0, __VA_ARGS__)

std::string vstrprintf(const char *format, va_list ap);
std::string real_strprintf(const std::string &format, int dummy, ...);
std::string FormatMoney(int64_t n, int64_t FractionalPartSize, bool fPlus=false);
bool parseMoneyValue(const char *value, const int64_t FractionalPartSize, int64_t *out);
