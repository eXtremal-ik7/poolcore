#pragma once

#include <inttypes.h>
#include <stdarg.h>
#include <string>

std::string real_strprintf(const std::string &format, int dummy, ...);
#define strprintf(format, ...) real_strprintf(format, 0, __VA_ARGS__)

std::string vstrprintf(const char *format, va_list ap);
std::string real_strprintf(const std::string &format, int dummy, ...);
std::string FormatMoney(int64_t n, int64_t rationalPartSize, bool fPlus=false);
bool parseMoneyValue(const char *value, const int64_t rationalPartSize, int64_t *out);
