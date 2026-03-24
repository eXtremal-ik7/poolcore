#include "gtest/gtest.h"
#include "idltool/jsonScanner.h"
#include "idltool/jsonWriteString.h"
#include "idltool/jsonWriteInt.h"
#include "idltool/jsonWriteUInt.h"
#include "idltool/jsonWriteDouble.h"
#include "idltool/jsonWriteBool.h"
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>

// ============================================================================
// Helpers
// ============================================================================



// ============================================================================
// Int64
// ============================================================================

TEST(JsonScanner, ReadInt) {
  auto read = [](const char *s) -> std::pair<bool, int64_t> {
    int64_t v = 0;
    const char *p = s, *end = s + strlen(s);
    return {jsonReadInt64(p, end, v) == JsonReadError::Ok, v};
  };

  // basic
  { auto [ok, v] = read("0"); ASSERT_TRUE(ok); EXPECT_EQ(v, 0); }
  { auto [ok, v] = read("42"); ASSERT_TRUE(ok); EXPECT_EQ(v, 42); }
  { auto [ok, v] = read("-1"); ASSERT_TRUE(ok); EXPECT_EQ(v, -1); }
  { auto [ok, v] = read("123456789"); ASSERT_TRUE(ok); EXPECT_EQ(v, 123456789LL); }

  // 18-digit fast path boundary
  { auto [ok, v] = read("999999999999999999"); ASSERT_TRUE(ok); EXPECT_EQ(v, 999999999999999999LL); }
  { auto [ok, v] = read("-999999999999999999"); ASSERT_TRUE(ok); EXPECT_EQ(v, -999999999999999999LL); }

  // 19-digit / limits
  { auto [ok, v] = read("9223372036854775807"); ASSERT_TRUE(ok); EXPECT_EQ(v, INT64_MAX); }
  { auto [ok, v] = read("-9223372036854775808"); ASSERT_TRUE(ok); EXPECT_EQ(v, INT64_MIN); }

  // overflow
  EXPECT_FALSE(read("9223372036854775808").first);
  EXPECT_FALSE(read("-9223372036854775809").first);
  EXPECT_FALSE(read("10000000000000000000").first);
  EXPECT_FALSE(read("100000000000000000000").first);

  // reject
  EXPECT_FALSE(read("\"abc\"").first);
  EXPECT_FALSE(read("true").first);
  EXPECT_FALSE(read("").first);

  // int32 range
  { int32_t v = 0; const char *p = "2147483647", *end = p + 10; EXPECT_EQ(jsonReadInt32(p, end, v), JsonReadError::Ok); EXPECT_EQ(v, INT32_MAX); }
  { int32_t v = 0; const char *p = "-2147483648", *end = p + 11; EXPECT_EQ(jsonReadInt32(p, end, v), JsonReadError::Ok); EXPECT_EQ(v, INT32_MIN); }
  { int32_t v = 0; const char *p = "2147483648", *end = p + 10; EXPECT_NE(jsonReadInt32(p, end, v), JsonReadError::Ok); }
}

TEST(JsonScanner, WriteInt) {
  auto write = [](int64_t v) { std::string s; jsonWriteInt(s, v); return s; };
  auto ref = [](int64_t v) { char b[24]; snprintf(b, sizeof(b), "%" PRId64, v); return std::string(b); };
  auto check = [&](int64_t v) { EXPECT_EQ(write(v), ref(v)) << v; };

  check(0); check(1); check(-1); check(42); check(-42);
  check(123456789); check(-123456789);
  check(999999999999999999LL); check(-999999999999999999LL);
  check(INT64_MAX); check(INT64_MIN);
  check(-4585056937269909242LL); check(4630517887836878971LL);
  for (int64_t v = 1; v <= 1000000000000000000LL; v *= 10) { check(v); check(-v); }
}

TEST(JsonScanner, RoundtripInt) {
  int64_t values[] = {0, 1, -1, INT64_MAX, INT64_MIN, -4585056937269909242LL, 4630517887836878971LL};
  for (int64_t v : values) {
    std::string buffer;
    jsonWriteInt(buffer, v);
    int64_t parsed = 0;
    const char *p = buffer.c_str(), *end = p + buffer.size();
    ASSERT_EQ(jsonReadInt64(p, end, parsed), JsonReadError::Ok) << v;
    EXPECT_EQ(v, parsed) << v;
  }
}

// ============================================================================
// UInt64
// ============================================================================

TEST(JsonScanner, ReadUInt) {
  auto read = [](const char *s) -> std::pair<bool, uint64_t> {
    uint64_t v = 0;
    const char *p = s, *end = s + strlen(s);
    return {jsonReadUInt64(p, end, v) == JsonReadError::Ok, v};
  };

  { auto [ok, v] = read("0"); ASSERT_TRUE(ok); EXPECT_EQ(v, 0u); }
  { auto [ok, v] = read("42"); ASSERT_TRUE(ok); EXPECT_EQ(v, 42u); }
  { auto [ok, v] = read("9999999999999999999"); ASSERT_TRUE(ok); EXPECT_EQ(v, 9999999999999999999ULL); }
  { auto [ok, v] = read("18446744073709551615"); ASSERT_TRUE(ok); EXPECT_EQ(v, UINT64_MAX); }
  { auto [ok, v] = read("10000000000000000000"); ASSERT_TRUE(ok); EXPECT_EQ(v, 10000000000000000000ULL); }

  // overflow
  EXPECT_FALSE(read("18446744073709551616").first);
  EXPECT_FALSE(read("20000000000000000000").first);
  EXPECT_FALSE(read("100000000000000000000").first);
  EXPECT_FALSE(read("-1").first);

  // uint32 range
  { uint32_t v = 0; const char *p = "4294967295", *end = p + 10; EXPECT_EQ(jsonReadUInt32(p, end, v), JsonReadError::Ok); EXPECT_EQ(v, UINT32_MAX); }
  { uint32_t v = 0; const char *p = "4294967296", *end = p + 10; EXPECT_NE(jsonReadUInt32(p, end, v), JsonReadError::Ok); }
}

TEST(JsonScanner, WriteUInt) {
  auto write = [](uint64_t v) { std::string s; jsonWriteUInt(s, v); return s; };
  auto ref = [](uint64_t v) { char b[24]; snprintf(b, sizeof(b), "%" PRIu64, v); return std::string(b); };
  auto check = [&](uint64_t v) { EXPECT_EQ(write(v), ref(v)) << v; };

  check(0); check(1); check(42); check(123456789);
  check(9999999999999999999ULL); check(10000000000000000000ULL); check(UINT64_MAX);
  for (uint64_t v = 1; v <= 10000000000000000000ULL; v *= 10) check(v);
}

TEST(JsonScanner, RoundtripUInt) {
  uint64_t values[] = {0, 1, UINT64_MAX, 10000000000000000000ULL};
  for (uint64_t v : values) {
    std::string buffer;
    jsonWriteUInt(buffer, v);
    uint64_t parsed = 0;
    const char *p = buffer.c_str(), *end = p + buffer.size();
    ASSERT_EQ(jsonReadUInt64(p, end, parsed), JsonReadError::Ok) << v;
    EXPECT_EQ(v, parsed) << v;
  }
}

// ============================================================================
// Double
// ============================================================================

TEST(JsonScanner, ReadDouble) {
  auto read = [](const char *s) -> std::pair<bool, double> {
    double v = 0;
    const char *p = s, *end = s + strlen(s);
    return {jsonReadDouble(p, end, v) == JsonReadError::Ok, v};
  };
  auto strtodRef = [](const char *s) { return std::strtod(s, nullptr); };
  auto checkStrtod = [&](const char *s) {
    auto [ok, v] = read(s);
    ASSERT_TRUE(ok) << s;
    EXPECT_DOUBLE_EQ(v, strtodRef(s)) << s;
  };

  // basic
  { auto [ok, v] = read("0"); ASSERT_TRUE(ok); EXPECT_DOUBLE_EQ(v, 0.0); }
  { auto [ok, v] = read("42"); ASSERT_TRUE(ok); EXPECT_DOUBLE_EQ(v, 42.0); }
  { auto [ok, v] = read("-3.14"); ASSERT_TRUE(ok); EXPECT_DOUBLE_EQ(v, -3.14); }
  { auto [ok, v] = read("0.123456789"); ASSERT_TRUE(ok); EXPECT_DOUBLE_EQ(v, 0.123456789); }

  // scientific
  { auto [ok, v] = read("1.5e10"); ASSERT_TRUE(ok); EXPECT_DOUBLE_EQ(v, 1.5e10); }
  { auto [ok, v] = read("2.5e-7"); ASSERT_TRUE(ok); EXPECT_DOUBLE_EQ(v, 2.5e-7); }
  { auto [ok, v] = read("3E5"); ASSERT_TRUE(ok); EXPECT_DOUBLE_EQ(v, 3e5); }
  { auto [ok, v] = read("-1.23e-4"); ASSERT_TRUE(ok); EXPECT_DOUBLE_EQ(v, -1.23e-4); }

  // extremes
  { auto [ok, v] = read("1.7976931348623157e+308"); ASSERT_TRUE(ok); EXPECT_DOUBLE_EQ(v, 1.7976931348623157e+308); }
  { auto [ok, v] = read("2.2250738585072014e-308"); ASSERT_TRUE(ok); EXPECT_GT(v, 0.0); }
  { auto [ok, v] = read("5e-300"); ASSERT_TRUE(ok); EXPECT_NEAR(v, 5e-300, 1e-315); }

  // >18 significant digits — match strtod
  checkStrtod("1234567890123456789");
  checkStrtod("0.0000000000000000001");
  { auto [ok, v] = read("-122.422003528379996"); ASSERT_TRUE(ok); EXPECT_NEAR(v, -122.422003528379996, 1e-12); }

  // reject
  EXPECT_FALSE(read("\"x\"").first);
  EXPECT_FALSE(read("").first);
}

TEST(JsonScanner, WriteDouble) {
  auto write = [](double v) { std::string s; jsonWriteDouble(s, v); return s; };
  auto ref = [](double v) { char b[64]; snprintf(b, sizeof(b), "%.12g", v); return std::string(b); };
  auto check = [&](double v) { EXPECT_EQ(write(v), ref(v)) << std::format("{:.17g}", v); };

  // basic
  check(0.0); check(1.0); check(-1.0); check(3.14); check(-3.14);
  check(0.1); check(0.01); check(0.001);

  // fixed/exponential boundary
  check(0.0001); check(0.00001);
  check(999999999999.0); check(1000000000000.0);

  // powers of 10
  for (int i = -307; i <= 308; i++) check(pow(10.0, i));

  // extremes
  check(1.7976931348623157e+308); check(5e-324); check(-0.0);

  // significant digits
  check(1.23456789012e100); check(1.23456789012e-200);
  check(-122.42200352825247); check(37.80848009696542);
}

TEST(JsonScanner, RoundtripDouble) {
  double values[] = {3.141592653589793, -122.42200352825247, 1e-10, 1e20, 0.0};
  for (double v : values) {
    std::string buffer;
    jsonWriteDouble(buffer, v);
    double parsed = 0;
    const char *p = buffer.c_str(), *end = p + buffer.size();
    ASSERT_EQ(jsonReadDouble(p, end, parsed), JsonReadError::Ok) << v;
    EXPECT_NEAR(v, parsed, std::abs(v) * 1e-10 + 1e-300) << v;
  }
}

// ============================================================================
// String
// ============================================================================

TEST(JsonScanner, ReadString) {
  auto read = [](const char *s) -> std::pair<bool, std::string> {
    std::string v;
    const char *p = s, *end = s + strlen(s);
    return {jsonReadStringValue(p, end, v) == JsonReadError::Ok, v};
  };

  // basic
  { auto [ok, v] = read("\"hello\""); ASSERT_TRUE(ok); EXPECT_EQ(v, "hello"); }
  { auto [ok, v] = read("\"\""); ASSERT_TRUE(ok); EXPECT_EQ(v, ""); }

  // escapes
  { auto [ok, v] = read(R"("a\nb")"); ASSERT_TRUE(ok); EXPECT_EQ(v, "a\nb"); }
  { auto [ok, v] = read(R"("a\tb")"); ASSERT_TRUE(ok); EXPECT_EQ(v, "a\tb"); }
  { auto [ok, v] = read(R"("a\"b")"); ASSERT_TRUE(ok); EXPECT_EQ(v, "a\"b"); }
  { auto [ok, v] = read(R"("a\\b")"); ASSERT_TRUE(ok); EXPECT_EQ(v, "a\\b"); }
  { auto [ok, v] = read(R"("a\/b")"); ASSERT_TRUE(ok); EXPECT_EQ(v, "a/b"); }
  { auto [ok, v] = read(R"("\b\f\r")"); ASSERT_TRUE(ok); EXPECT_EQ(v, "\b\f\r"); }

  // unicode: U+00E9 → 2-byte UTF-8, U+4E16 → 3-byte, surrogate pair U+1F600 → 4-byte
  { auto [ok, v] = read(R"("\u00e9")"); ASSERT_TRUE(ok); EXPECT_EQ(v, "\xC3\xA9"); }
  { auto [ok, v] = read(R"("\u4e16")"); ASSERT_TRUE(ok); EXPECT_EQ(v, "\xE4\xB8\x96"); }
  { auto [ok, v] = read(R"("\uD83D\uDE00")"); ASSERT_TRUE(ok); EXPECT_EQ(v, "\xF0\x9F\x98\x80"); }

  // reject
  EXPECT_FALSE(read("").first);
  EXPECT_FALSE(read("noquotes").first);
  EXPECT_FALSE(read("\"unterminated").first);
}

TEST(JsonScanner, WriteString) {
  auto write = [](std::string_view v) { std::string s; jsonWriteString(s, v); return s; };

  EXPECT_EQ(write("hello"), "\"hello\"");
  EXPECT_EQ(write(""), "\"\"");
  EXPECT_EQ(write("a\"b"), "\"a\\\"b\"");
  EXPECT_EQ(write("a\\b"), "\"a\\\\b\"");
  EXPECT_EQ(write("a\nb"), "\"a\\nb\"");
  EXPECT_EQ(write("a\tb"), "\"a\\tb\"");
  EXPECT_EQ(write("\b\f\r"), "\"\\b\\f\\r\"");
  EXPECT_EQ(write(std::string_view("\x01", 1)), "\"\\u0001\"");
}

TEST(JsonScanner, RoundtripString) {
  const char *values[] = {"", "hello", "line1\nline2\ttab\"quote\\backslash", "\xC3\xA9"};
  for (const char *v : values) {
    std::string buffer;
    jsonWriteString(buffer, v);
    std::string parsed;
    const char *p = buffer.c_str(), *end = p + buffer.size();
    ASSERT_EQ(jsonReadStringValue(p, end, parsed), JsonReadError::Ok) << v;
    EXPECT_EQ(parsed, v);
  }
}

// ============================================================================
// Bool
// ============================================================================

TEST(JsonScanner, ReadBool) {
  auto read = [](const char *s) -> std::pair<bool, bool> {
    bool v = false;
    const char *p = s, *end = s + strlen(s);
    return {jsonReadBool(p, end, v) == JsonReadError::Ok, v};
  };

  { auto [ok, v] = read("true"); ASSERT_TRUE(ok); EXPECT_TRUE(v); }
  { auto [ok, v] = read("false"); ASSERT_TRUE(ok); EXPECT_FALSE(v); }
  EXPECT_FALSE(read("1").first);
  EXPECT_FALSE(read("\"true\"").first);
  EXPECT_FALSE(read("").first);
}

TEST(JsonScanner, WriteBool) {
  { std::string s; jsonWriteBool(s, true); EXPECT_EQ(s, "true"); }
  { std::string s; jsonWriteBool(s, false); EXPECT_EQ(s, "false"); }
}
