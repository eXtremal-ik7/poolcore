#include "poolcommon/utils.h"


std::string vstrprintf(const char *format, va_list ap)
{
    char buffer[50000];
    char* p = buffer;
    int limit = sizeof(buffer);
    int ret;
    for (;;)
    {
        va_list arg_ptr;
        va_copy(arg_ptr, ap);
#ifdef WIN32
        ret = _vsnprintf(p, limit, format, arg_ptr);
#else
        ret = vsnprintf(p, limit, format, arg_ptr);
#endif
        va_end(arg_ptr);
        if (ret >= 0 && ret < limit)
            break;
        if (p != buffer)
            delete[] p;
        limit *= 2;
        p = new char[limit];
        if (p == NULL)
            throw std::bad_alloc();
    }
    std::string str(p, p+ret);
    if (p != buffer)
        delete[] p;
    return str;
}

std::string real_strprintf(const std::string &format, int dummy, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, dummy);
    std::string str = vstrprintf(format.c_str(), arg_ptr);
    va_end(arg_ptr);
    return str;
}

std::string FormatMoney(int64_t n, int64_t rationalPartSize, bool fPlus)
{
    // Note: not using straight sprintf here because we do NOT want
    // localized number formatting.
    int64_t n_abs = (n > 0 ? n : -n);
    int64_t quotient = n_abs/rationalPartSize;
    int64_t remainder = n_abs%rationalPartSize;
    std::string str = strprintf("%" PRId64 ".%08" PRId64, quotient, remainder);

    // Right-trim excess zeros before the decimal point:
    int nTrim = 0;
    for (int i = static_cast<int>(str.size())-1; (str[i] == '0' && isdigit(str[i-2])); --i)
        ++nTrim;
    if (nTrim)
        str.erase(str.size()-nTrim, nTrim);

    if (n < 0)
        str.insert((unsigned int)0, 1, '-');
    else if (fPlus && n > 0)
        str.insert((unsigned int)0, 1, '+');
    return str;
}

bool parseMoneyValue(const char *value, const int64_t rationalPartSize, int64_t *out)
{
  *out = 0;
  int64_t fractionalMultiplier = rationalPartSize;
  int64_t rationalPart = 0;
  int64_t fractionalPart = 0;
  const char *p = value;
  char s;

  // Parse rational part
  if (*p == 0)
    return false;
  for (;; p++) {
    s = *p;
    if (s >= '0' && s <= '9') {
      rationalPart *= 10;
      rationalPart += s - '0';
    } else if (s == '.') {
      break;
    } else if (s == '\0') {
      *out = rationalPart * rationalPartSize;
      return true;
    } else {
      return false;
    }
  }

  // Parse fractional part
  p++;
  for (;; p++) {
    s = *p;
    if (s >= '0' && s <= '9') {
      fractionalPart *= 10;
      fractionalPart += s - '0';
      fractionalMultiplier /= 10;
      if (fractionalMultiplier == 0)
        return false;
    } else if (s == '\0') {
      break;
    } else {
      return false;
    }
  }

  *out = rationalPart*rationalPartSize + fractionalPart*fractionalMultiplier;
  return true;
}
