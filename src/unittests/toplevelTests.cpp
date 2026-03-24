#include "gtest/gtest.h"
#include "test.idl.h"
#include <cstring>
#include <string>

// ============================================================================
// Toplevel map
// ============================================================================

TEST(IdlTool, ParseTopLevelMap) {
  const char *json = R"({"alice":{"value":"hello","count":1},"bob":{"value":"world","count":2}})";
  TopLevelMap t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.data.size(), 2u);
  EXPECT_EQ(t.data.at("alice").value, "hello");
  EXPECT_EQ(t.data.at("alice").count, 1);
  EXPECT_EQ(t.data.at("bob").value, "world");
  EXPECT_EQ(t.data.at("bob").count, 2);
}

TEST(IdlTool, ParseTopLevelMapEmpty) {
  const char *json = R"({})";
  TopLevelMap t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_TRUE(t.data.empty());
}

TEST(IdlTool, ParseTopLevelMapInvalid) {
  const char *json = R"([1,2,3])";
  TopLevelMap t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(IdlTool, ParseTopLevelMapVerbose) {
  const char *json = R"({"x":{"value":"v","count":5}})";
  TopLevelMap t;
  ParseError error;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), error));
  EXPECT_EQ(t.data.at("x").count, 5);
}

TEST(IdlTool, ParseTopLevelMapVerboseInvalid) {
  const char *json = R"([1,2])";
  TopLevelMap t;
  ParseError error;
  EXPECT_FALSE(t.parseVerbose(json, strlen(json), error));
}

TEST(IdlTool, SerializeTopLevelMap) {
  TopLevelMap t;
  t.data["k"] = Inner{"val", 7};
  std::string stream;
  t.serialize(stream);
  std::string s = stream;
  TopLevelMap t2;
  ASSERT_TRUE(t2.parse(s.data(), s.size()));
  EXPECT_EQ(t2.data.at("k").value, "val");
  EXPECT_EQ(t2.data.at("k").count, 7);
}

// ============================================================================
// Toplevel array
// ============================================================================

TEST(IdlTool, ParseTopLevelArray) {
  const char *json = R"([{"value":"a","count":1},{"value":"b","count":2}])";
  TopLevelArray t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.items.size(), 2u);
  EXPECT_EQ(t.items[0].value, "a");
  EXPECT_EQ(t.items[1].value, "b");
  EXPECT_EQ(t.items[1].count, 2);
}

TEST(IdlTool, ParseTopLevelArrayEmpty) {
  const char *json = R"([])";
  TopLevelArray t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_TRUE(t.items.empty());
}

TEST(IdlTool, ParseTopLevelArrayInvalid) {
  const char *json = R"({"not":"array"})";
  TopLevelArray t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(IdlTool, ParseTopLevelArrayVerbose) {
  const char *json = R"([{"value":"x","count":9}])";
  TopLevelArray t;
  ParseError error;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), error));
  ASSERT_EQ(t.items.size(), 1u);
  EXPECT_EQ(t.items[0].count, 9);
}

TEST(IdlTool, SerializeTopLevelArray) {
  TopLevelArray t;
  t.items.push_back(Inner{"a", 1});
  t.items.push_back(Inner{"b", 2});
  std::string stream;
  t.serialize(stream);
  std::string s = stream;
  TopLevelArray t2;
  ASSERT_TRUE(t2.parse(s.data(), s.size()));
  ASSERT_EQ(t2.items.size(), 2u);
  EXPECT_EQ(t2.items[0].value, "a");
  EXPECT_EQ(t2.items[1].count, 2);
}

// ============================================================================
// Toplevel scalar
// ============================================================================

TEST(IdlTool, ParseTopLevelScalar) {
  const char *json = R"(3.14)";
  TopLevelScalar t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_DOUBLE_EQ(t.value, 3.14);
}

TEST(IdlTool, ParseTopLevelScalarInvalid) {
  const char *json = R"("not a number")";
  TopLevelScalar t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(IdlTool, ParseTopLevelScalarVerbose) {
  const char *json = R"(2.718)";
  TopLevelScalar t;
  ParseError error;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), error));
  EXPECT_DOUBLE_EQ(t.value, 2.718);
}
