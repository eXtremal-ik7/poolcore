#include "gtest/gtest.h"
#include "test.idl.h"
#include "p2putils/xmstream.h"
#include "rapidjson/document.h"
#include <cmath>
#include <cstring>
#include <string>

static rapidjson::Document parseRapidArray(const xmstream &stream) {
  rapidjson::Document doc;
  doc.Parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf());
  return doc;
}

// ============================================================================
// Parse tests
// ============================================================================

TEST(ArrayLayout, ParseAllRequired) {
  const char *json = "[1,2,3]";
  ArrayLayoutAllRequired t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.x, 1);
  EXPECT_EQ(t.y, 2);
  EXPECT_EQ(t.z, 3);
}

TEST(ArrayLayout, ParseAllRequiredWithWhitespace) {
  const char *json = "[ 10 , 20 , 30 ]";
  ArrayLayoutAllRequired t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.x, 10);
  EXPECT_EQ(t.y, 20);
  EXPECT_EQ(t.z, 30);
}

TEST(ArrayLayout, ParseMissingRequired) {
  const char *json = "[1]";
  ArrayLayoutAllRequired t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(ArrayLayout, ParseMixedFull) {
  const char *json = R"(["hello",42,"custom",1.5,"extra"])";
  ArrayLayoutMixed t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.name, "hello");
  EXPECT_EQ(t.count, 42);
  EXPECT_EQ(t.label, "custom");
  EXPECT_DOUBLE_EQ(t.score, 1.5);
  ASSERT_TRUE(t.extra.has_value());
  EXPECT_EQ(*t.extra, "extra");
}

TEST(ArrayLayout, ParseMixedTrailingDefaults) {
  const char *json = R"(["hello",42])";
  ArrayLayoutMixed t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.name, "hello");
  EXPECT_EQ(t.count, 42);
  EXPECT_EQ(t.label, "default");
  EXPECT_DOUBLE_EQ(t.score, 0.0);
  EXPECT_FALSE(t.extra.has_value());
}

TEST(ArrayLayout, ParseMixedPartial) {
  const char *json = R"(["hello",42,"custom"])";
  ArrayLayoutMixed t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.name, "hello");
  EXPECT_EQ(t.count, 42);
  EXPECT_EQ(t.label, "custom");
  EXPECT_DOUBLE_EQ(t.score, 0.0);
  EXPECT_FALSE(t.extra.has_value());
}

TEST(ArrayLayout, ParseMixedMissingRequired) {
  const char *json = R"(["hello"])";
  ArrayLayoutMixed t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(ArrayLayout, ParseNested) {
  const char *json = R"([42,{"value":"inner","count":7},[1,2,3]])";
  ArrayLayoutNested t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.id, 42);
  EXPECT_EQ(t.child.value, "inner");
  EXPECT_EQ(t.child.count, 7);
  ASSERT_EQ(t.items.size(), 3u);
  EXPECT_EQ(t.items[0], 1);
  EXPECT_EQ(t.items[1], 2);
  EXPECT_EQ(t.items[2], 3);
}

TEST(ArrayLayout, ParseEmptyArray) {
  // All fields have defaults
  const char *json = "[]";
  ArrayLayoutAllDefaults t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.x, 0);
  EXPECT_EQ(t.y, 0);
}

TEST(ArrayLayout, ParseAllDefaultsPartial) {
  const char *json = "[5]";
  ArrayLayoutAllDefaults t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.x, 5);
  EXPECT_EQ(t.y, 0);
}

TEST(ArrayLayout, ParseSkipExtra) {
  const char *json = R"(["hello",42,99,"extra",true])";
  ArrayLayoutSkipExtra t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.a, "hello");
  EXPECT_EQ(t.b, 42);
}

TEST(ArrayLayout, ParseExtraWithoutSkipFails) {
  // ArrayLayoutAllRequired does NOT have skip_unknown
  const char *json = "[1,2,3,4]";
  ArrayLayoutAllRequired t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(ArrayLayout, ParseEnum) {
  const char *json = R"(["blue"])";
  ArrayLayoutEnum t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.color, EColor::blue);
  EXPECT_EQ(t.priority, EPriority::medium); // default
}

TEST(ArrayLayout, ParseEnumOverride) {
  const char *json = R"(["red","high"])";
  ArrayLayoutEnum t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.color, EColor::red);
  EXPECT_EQ(t.priority, EPriority::high);
}

// ============================================================================
// Serialize tests
// ============================================================================

TEST(ArrayLayout, SerializeAllRequired) {
  ArrayLayoutAllRequired t;
  t.x = 1; t.y = 2; t.z = 3;
  xmstream stream;
  t.serialize(stream);
  auto doc = parseRapidArray(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.IsArray());
  ASSERT_EQ(doc.Size(), 3u);
  EXPECT_EQ(doc[0].GetInt(), 1);
  EXPECT_EQ(doc[1].GetInt(), 2);
  EXPECT_EQ(doc[2].GetInt(), 3);
}

TEST(ArrayLayout, SerializeDefaultsTrimmed) {
  ArrayLayoutMixed t;
  t.name = "a";
  t.count = 1;
  // label, score, extra all at defaults
  xmstream stream;
  t.serialize(stream);
  auto doc = parseRapidArray(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.IsArray());
  EXPECT_EQ(doc.Size(), 2u);
  EXPECT_STREQ(doc[0].GetString(), "a");
  EXPECT_EQ(doc[1].GetInt64(), 1);
}

TEST(ArrayLayout, SerializeMiddleNonDefault) {
  ArrayLayoutMixed t;
  t.name = "a";
  t.count = 1;
  t.label = "default"; // default value
  t.score = 9.9; // non-default — forces label to be serialized too
  xmstream stream;
  t.serialize(stream);
  auto doc = parseRapidArray(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.IsArray());
  EXPECT_EQ(doc.Size(), 4u);
  EXPECT_STREQ(doc[0].GetString(), "a");
  EXPECT_EQ(doc[1].GetInt64(), 1);
  EXPECT_STREQ(doc[2].GetString(), "default");
  EXPECT_DOUBLE_EQ(doc[3].GetDouble(), 9.9);
}

TEST(ArrayLayout, SerializeOptionalInMiddle) {
  ArrayLayoutMixed t;
  t.name = "a";
  t.count = 1;
  // label and score at defaults, but extra is set — forces all to be emitted
  t.extra = "yes";
  xmstream stream;
  t.serialize(stream);
  auto doc = parseRapidArray(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.IsArray());
  EXPECT_EQ(doc.Size(), 5u);
  EXPECT_STREQ(doc[0].GetString(), "a");
  EXPECT_EQ(doc[1].GetInt64(), 1);
  EXPECT_STREQ(doc[2].GetString(), "default"); // default value forced out
  EXPECT_DOUBLE_EQ(doc[3].GetDouble(), 0.0);   // default value forced out
  EXPECT_STREQ(doc[4].GetString(), "yes");
}

TEST(ArrayLayout, SerializeNested) {
  ArrayLayoutNested t;
  t.id = 42;
  t.child.value = "inner";
  t.child.count = 7;
  t.items = {1, 2, 3};
  xmstream stream;
  t.serialize(stream);
  auto doc = parseRapidArray(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.IsArray());
  ASSERT_EQ(doc.Size(), 3u);
  EXPECT_EQ(doc[0].GetInt64(), 42);
  ASSERT_TRUE(doc[1].IsObject());
  EXPECT_STREQ(doc[1]["value"].GetString(), "inner");
  ASSERT_TRUE(doc[2].IsArray());
  EXPECT_EQ(doc[2].Size(), 3u);
}

TEST(ArrayLayout, SerializeAllDefaultsEmpty) {
  ArrayLayoutAllDefaults t; // x=0, y=0 — all at defaults
  xmstream stream;
  t.serialize(stream);
  std::string output(reinterpret_cast<const char*>(stream.data()), stream.sizeOf());
  EXPECT_EQ(output, "[]");
}

TEST(ArrayLayout, SerializeEnum) {
  ArrayLayoutEnum t;
  t.color = EColor::green;
  // priority stays at default "medium"
  xmstream stream;
  t.serialize(stream);
  auto doc = parseRapidArray(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.IsArray());
  EXPECT_EQ(doc.Size(), 1u); // trailing default trimmed
  EXPECT_STREQ(doc[0].GetString(), "green");
}

TEST(ArrayLayout, SerializeEnumNonDefault) {
  ArrayLayoutEnum t;
  t.color = EColor::blue;
  t.priority = EPriority::critical;
  xmstream stream;
  t.serialize(stream);
  auto doc = parseRapidArray(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.IsArray());
  EXPECT_EQ(doc.Size(), 2u);
  EXPECT_STREQ(doc[0].GetString(), "blue");
  EXPECT_STREQ(doc[1].GetString(), "critical");
}

// ============================================================================
// Roundtrip tests
// ============================================================================

TEST(ArrayLayout, RoundtripAllRequired) {
  ArrayLayoutAllRequired original;
  original.x = -5; original.y = 0; original.z = 999;
  xmstream stream;
  original.serialize(stream);
  ArrayLayoutAllRequired parsed;
  ASSERT_TRUE(parsed.parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf()));
  EXPECT_EQ(parsed.x, original.x);
  EXPECT_EQ(parsed.y, original.y);
  EXPECT_EQ(parsed.z, original.z);
}

TEST(ArrayLayout, RoundtripMixed) {
  ArrayLayoutMixed original;
  original.name = "test";
  original.count = 42;
  original.label = "custom";
  original.score = 3.14;
  original.extra = "data";
  xmstream stream;
  original.serialize(stream);
  ArrayLayoutMixed parsed;
  ASSERT_TRUE(parsed.parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf()));
  EXPECT_EQ(parsed.name, original.name);
  EXPECT_EQ(parsed.count, original.count);
  EXPECT_EQ(parsed.label, original.label);
  EXPECT_DOUBLE_EQ(parsed.score, original.score);
  ASSERT_TRUE(parsed.extra.has_value());
  EXPECT_EQ(*parsed.extra, *original.extra);
}

TEST(ArrayLayout, RoundtripMixedDefaults) {
  ArrayLayoutMixed original;
  original.name = "test";
  original.count = 42;
  // trailing fields at defaults
  xmstream stream;
  original.serialize(stream);
  ArrayLayoutMixed parsed;
  ASSERT_TRUE(parsed.parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf()));
  EXPECT_EQ(parsed.name, original.name);
  EXPECT_EQ(parsed.count, original.count);
  EXPECT_EQ(parsed.label, "default");
  EXPECT_DOUBLE_EQ(parsed.score, 0.0);
  EXPECT_FALSE(parsed.extra.has_value());
}

TEST(ArrayLayout, RoundtripNested) {
  ArrayLayoutNested original;
  original.id = 100;
  original.child.value = "nested";
  original.child.count = 55;
  original.items = {10, 20, 30};
  xmstream stream;
  original.serialize(stream);
  ArrayLayoutNested parsed;
  ASSERT_TRUE(parsed.parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf()));
  EXPECT_EQ(parsed.id, original.id);
  EXPECT_EQ(parsed.child.value, original.child.value);
  EXPECT_EQ(parsed.child.count, original.child.count);
  EXPECT_EQ(parsed.items, original.items);
}

// ============================================================================
// Verbose parse tests
// ============================================================================

TEST(ArrayLayout, VerboseParseAllRequired) {
  const char *json = "[1,2,3]";
  ArrayLayoutAllRequired t;
  ParseError error;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), error));
  EXPECT_EQ(t.x, 1);
  EXPECT_EQ(t.y, 2);
  EXPECT_EQ(t.z, 3);
}

TEST(ArrayLayout, VerboseParseMixed) {
  const char *json = R"(["hello",42,"label"])";
  ArrayLayoutMixed t;
  ParseError error;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), error));
  EXPECT_EQ(t.name, "hello");
  EXPECT_EQ(t.count, 42);
  EXPECT_EQ(t.label, "label");
}

TEST(ArrayLayout, VerboseParseMissingRequired) {
  const char *json = R"(["hello"])";
  ArrayLayoutMixed t;
  ParseError error;
  EXPECT_FALSE(t.parseVerbose(json, strlen(json), error));
  // Should report missing required field 'count' at index 1
  EXPECT_FALSE(error.message.empty());
}

TEST(ArrayLayout, VerboseParseSkipExtra) {
  const char *json = R"(["hello",42,99,"extra"])";
  ArrayLayoutSkipExtra t;
  ParseError error;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), error));
  EXPECT_EQ(t.a, "hello");
  EXPECT_EQ(t.b, 42);
}

// ============================================================================
// Barrier: empty optional with null_in=deny truncates the array
// ============================================================================

TEST(ArrayLayout, BarrierTruncatesAtEmptyDenyOptional) {
  // opt1 is empty (null_in=deny), opt2 has a value.
  // Serialize should truncate at opt1 — can't emit null (parser would reject).
  ArrayLayoutBarrier t;
  t.name = "test";
  // opt1 empty (nullopt), opt2 has a value
  t.opt2 = "after";
  xmstream stream;
  t.serialize(stream);
  std::string output(reinterpret_cast<const char*>(stream.data()), stream.sizeOf());
  // Should be just ["test"] — opt1 empty with deny, so truncate there
  EXPECT_EQ(output, R"(["test"])");
}

TEST(ArrayLayout, BarrierNotTriggeredWhenPresent) {
  // opt1 has a value — no barrier, opt2 also serialized
  ArrayLayoutBarrier t;
  t.name = "test";
  t.opt1 = "first";
  t.opt2 = "second";
  xmstream stream;
  t.serialize(stream);
  auto doc = parseRapidArray(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.IsArray());
  EXPECT_EQ(doc.Size(), 3u);
  EXPECT_STREQ(doc[0].GetString(), "test");
  EXPECT_STREQ(doc[1].GetString(), "first");
  EXPECT_STREQ(doc[2].GetString(), "second");
}

TEST(ArrayLayout, BarrierRoundtrip) {
  // opt1 has a value, opt2 empty — serialize includes opt1, opt2 as null
  ArrayLayoutBarrier t;
  t.name = "test";
  t.opt1 = "first";
  // opt2 empty — null_in=allow, so null is OK
  xmstream stream;
  t.serialize(stream);
  ArrayLayoutBarrier parsed;
  ASSERT_TRUE(parsed.parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf()));
  EXPECT_EQ(parsed.name, "test");
  ASSERT_TRUE(parsed.opt1.has_value());
  EXPECT_EQ(*parsed.opt1, "first");
  EXPECT_FALSE(parsed.opt2.has_value());
}

TEST(ArrayLayout, BarrierAllEmptyTruncatesAll) {
  // Both optionals empty. opt1 is deny, so truncate immediately.
  ArrayLayoutBarrier t;
  t.name = "test";
  xmstream stream;
  t.serialize(stream);
  std::string output(reinterpret_cast<const char*>(stream.data()), stream.sizeOf());
  EXPECT_EQ(output, R"(["test"])");
}

// ============================================================================
// Optional sub-object in array_layout
// ============================================================================

TEST(ArrayLayout, OptionalObjectPresent) {
  const char *json = R"(["hello",{"value":"v","count":5}])";
  ArrayLayoutOptionalObject t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.name, "hello");
  ASSERT_TRUE(t.child.has_value());
  EXPECT_EQ(t.child->value, "v");
  EXPECT_EQ(t.child->count, 5);
}

TEST(ArrayLayout, OptionalObjectAbsent) {
  const char *json = R"(["hello"])";
  ArrayLayoutOptionalObject t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.name, "hello");
  EXPECT_FALSE(t.child.has_value());
}

TEST(ArrayLayout, OptionalObjectSerializePresent) {
  ArrayLayoutOptionalObject t;
  t.name = "hi";
  t.child.emplace();
  t.child->value = "x";
  t.child->count = 3;
  xmstream stream;
  t.serialize(stream);
  std::string output(reinterpret_cast<const char*>(stream.data()), stream.sizeOf());
  EXPECT_EQ(output, R"(["hi",{"value":"x","count":3}])");
}

TEST(ArrayLayout, OptionalObjectSerializeAbsent) {
  ArrayLayoutOptionalObject t;
  t.name = "hi";
  xmstream stream;
  t.serialize(stream);
  std::string output(reinterpret_cast<const char*>(stream.data()), stream.sizeOf());
  EXPECT_EQ(output, R"(["hi"])");
}

TEST(ArrayLayout, OptionalObjectRoundtrip) {
  ArrayLayoutOptionalObject t;
  t.name = "rt";
  t.child.emplace();
  t.child->value = "rval";
  t.child->count = 42;
  xmstream stream;
  t.serialize(stream);

  ArrayLayoutOptionalObject parsed;
  ASSERT_TRUE(parsed.parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf()));
  EXPECT_EQ(parsed.name, "rt");
  ASSERT_TRUE(parsed.child.has_value());
  EXPECT_EQ(parsed.child->value, "rval");
  EXPECT_EQ(parsed.child->count, 42);
}
