#include "gtest/gtest.h"
#include "test.idl.h"
#include "rapidjson/document.h"

static rapidjson::Document parseRapid(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str(), json.size());
  return doc;
}

// ============================================================================
// Parse tests
// ============================================================================

TEST(IdlTool, ParseScalarTypes) {
  const char *json = R"({"fieldString":"hello","fieldBool":true,"fieldInt32":42,"fieldUint32":100,"fieldInt64":999,"fieldUint64":1234,"fieldDouble":3.14})";
  ScalarTypes t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.fieldString, "hello");
  EXPECT_EQ(t.fieldBool, true);
  EXPECT_EQ(t.fieldInt32, 42);
  EXPECT_EQ(t.fieldUint32, 100u);
  EXPECT_EQ(t.fieldInt64, 999);
  EXPECT_EQ(t.fieldUint64, 1234u);
  EXPECT_DOUBLE_EQ(t.fieldDouble, 3.14);
}

TEST(IdlTool, ParseMissingRequired) {
  const char *json = R"({"fieldString":"hi"})";
  ScalarTypes t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(IdlTool, ParseDefaults) {
  ScalarDefaults d;
  const char *json = R"({})";
  ASSERT_TRUE(d.parse(json, strlen(json)));
  EXPECT_EQ(d.fieldString, "hello");
  EXPECT_EQ(d.fieldBool, true);
  EXPECT_EQ(d.fieldInt32, -1);
  EXPECT_EQ(d.fieldUint32, 42u);
  EXPECT_EQ(d.fieldInt64, 0);
  EXPECT_EQ(d.fieldUint64, 100u);
  EXPECT_DOUBLE_EQ(d.fieldDouble, 3.14);
}

TEST(IdlTool, ParseDefaultsOverride) {
  const char *json = R"({"fieldString":"world","fieldBool":false,"fieldInt32":99})";
  ScalarDefaults d;
  ASSERT_TRUE(d.parse(json, strlen(json)));
  EXPECT_EQ(d.fieldString, "world");
  EXPECT_EQ(d.fieldBool, false);
  EXPECT_EQ(d.fieldInt32, 99);
  EXPECT_EQ(d.fieldUint32, 42u);
}

TEST(IdlTool, ParseEnum) {
  const char *json = R"({"color":"blue","priority":"critical"})";
  WithEnum we;
  ASSERT_TRUE(we.parse(json, strlen(json)));
  EXPECT_EQ(we.color, EColor::blue);
  EXPECT_EQ(we.priority, EPriority::critical);
}

TEST(IdlTool, ParseEnumDefault) {
  const char *json = R"({"color":"red"})";
  WithEnum we;
  ASSERT_TRUE(we.parse(json, strlen(json)));
  EXPECT_EQ(we.color, EColor::red);
  EXPECT_EQ(we.priority, EPriority::medium);
}

TEST(IdlTool, ParseEnumInvalid) {
  const char *json = R"({"color":"pink"})";
  WithEnum we;
  EXPECT_FALSE(we.parse(json, strlen(json)));
}

TEST(IdlTool, ParseNested) {
  const char *json = R"({"label":"test","child":{"value":"inner","count":5}})";
  Outer o;
  ASSERT_TRUE(o.parse(json, strlen(json)));
  EXPECT_EQ(o.label, "test");
  EXPECT_EQ(o.child.value, "inner");
  EXPECT_EQ(o.child.count, 5);
}

TEST(IdlTool, ParseOptionalObject) {
  const char *json1 = R"({"name":"x","first":{"value":"a","count":1}})";
  OptionalChildren oc1;
  ASSERT_TRUE(oc1.parse(json1, strlen(json1)));
  EXPECT_EQ(oc1.name, "x");
  ASSERT_TRUE(oc1.first.has_value());
  EXPECT_EQ(oc1.first->value, "a");
  EXPECT_EQ(oc1.first->count, 1);
  EXPECT_FALSE(oc1.second.has_value());

  const char *json2 = R"({"name":"y","first":null,"second":null})";
  OptionalChildren oc2;
  ASSERT_TRUE(oc2.parse(json2, strlen(json2)));
  EXPECT_FALSE(oc2.first.has_value());
  EXPECT_FALSE(oc2.second.has_value());
}

TEST(IdlTool, ParseArray) {
  const char *json = R"({"strings":["a","b","c"],"numbers":[1,2,3],"doubles":[1.1,2.2],"items":[{"value":"x","count":10}]})";
  ArrayFields af;
  ASSERT_TRUE(af.parse(json, strlen(json)));
  ASSERT_EQ(af.strings.size(), 3u);
  EXPECT_EQ(af.strings[0], "a");
  EXPECT_EQ(af.strings[2], "c");
  ASSERT_EQ(af.numbers.size(), 3u);
  EXPECT_EQ(af.numbers[1], 2);
  ASSERT_EQ(af.doubles.size(), 2u);
  EXPECT_DOUBLE_EQ(af.doubles[0], 1.1);
  ASSERT_EQ(af.items.size(), 1u);
  EXPECT_EQ(af.items[0].value, "x");
  EXPECT_EQ(af.items[0].count, 10);
}

TEST(IdlTool, ParseOptionalArray) {
  const char *json1 = R"({"label":"x","tags":["a","b"],"items":[{"value":"v","count":1}]})";
  OptionalArrays oa1;
  ASSERT_TRUE(oa1.parse(json1, strlen(json1)));
  ASSERT_TRUE(oa1.tags.has_value());
  EXPECT_EQ(oa1.tags->size(), 2u);
  ASSERT_TRUE(oa1.items.has_value());
  EXPECT_EQ(oa1.items->size(), 1u);

  const char *json2 = R"({"label":"y","tags":null})";
  OptionalArrays oa2;
  ASSERT_TRUE(oa2.parse(json2, strlen(json2)));
  EXPECT_FALSE(oa2.tags.has_value());
  EXPECT_FALSE(oa2.items.has_value());

  const char *json3 = R"({"label":"z"})";
  OptionalArrays oa3;
  ASSERT_TRUE(oa3.parse(json3, strlen(json3)));
  EXPECT_FALSE(oa3.tags.has_value());
}

TEST(IdlTool, ParseMixin) {
  const char *json = R"({"id":"abc","value":2.5})";
  WithMixin wm;
  ASSERT_TRUE(wm.parse(json, strlen(json)));
  EXPECT_EQ(wm.id, "abc");
  EXPECT_EQ(wm.name, "");
  EXPECT_DOUBLE_EQ(wm.value, 2.5);
}

TEST(IdlTool, ParseMultipleMixins) {
  const char *json = R"({"id":"1","createdAt":1000,"payload":"data"})";
  WithMultipleMixins wmm;
  ASSERT_TRUE(wmm.parse(json, strlen(json)));
  EXPECT_EQ(wmm.id, "1");
  EXPECT_EQ(wmm.createdAt, 1000);
  EXPECT_EQ(wmm.updatedAt, 0);
  EXPECT_EQ(wmm.payload, "data");
}

TEST(IdlTool, ParseDeepNesting) {
  const char *json = R"({"child":{"nested":{"value":42},"items":[{"value":1},{"value":2}]}})";
  Level0 l0;
  ASSERT_TRUE(l0.parse(json, strlen(json)));
  EXPECT_EQ(l0.child.nested.value, 42);
  ASSERT_EQ(l0.child.items.size(), 2u);
  EXPECT_EQ(l0.child.items[0].value, 1);
  EXPECT_EQ(l0.child.items[1].value, 2);
}

TEST(IdlTool, ParseUnknownFieldsIgnored) {
  const char *json = R"({"value":"x","count":1,"extra":"ignored","nested":{"a":1}})";
  Inner inner;
  ASSERT_TRUE(inner.parse(json, strlen(json)));
  EXPECT_EQ(inner.value, "x");
  EXPECT_EQ(inner.count, 1);
}

TEST(IdlTool, ParseEmptyObject) {
  const char *json = R"({})";
  ScalarDefaults d;
  ASSERT_TRUE(d.parse(json, strlen(json)));
}

TEST(IdlTool, ParseInvalidJson) {
  const char *bad1 = R"(not json)";
  ScalarDefaults d;
  EXPECT_FALSE(d.parse(bad1, strlen(bad1)));

  const char *bad2 = R"({"fieldString":})";
  ScalarTypes t;
  EXPECT_FALSE(t.parse(bad2, strlen(bad2)));
}

TEST(IdlTool, ParseStringEscapes) {
  const char *json = R"({"value":"hello\nworld","count":1})";
  Inner inner;
  ASSERT_TRUE(inner.parse(json, strlen(json)));
  EXPECT_EQ(inner.value, "hello\nworld");
}

// ============================================================================
// Serialize tests — verify via rapidjson
// ============================================================================

TEST(IdlTool, SerializeScalarTypes) {
  ScalarTypes t;
  t.fieldString = "test";
  t.fieldBool = false;
  t.fieldInt32 = -5;
  t.fieldUint32 = 10;
  t.fieldInt64 = 9999;
  t.fieldUint64 = 12345;
  t.fieldDouble = 2.718;

  auto doc = parseRapid(t.serialize());
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["fieldString"].GetString(), "test");
  EXPECT_EQ(doc["fieldBool"].GetBool(), false);
  EXPECT_EQ(doc["fieldInt32"].GetInt(), -5);
  EXPECT_EQ(doc["fieldUint32"].GetUint(), 10u);
  EXPECT_EQ(doc["fieldInt64"].GetInt64(), 9999);
  EXPECT_EQ(doc["fieldUint64"].GetUint64(), 12345u);
  EXPECT_DOUBLE_EQ(doc["fieldDouble"].GetDouble(), 2.718);
}

TEST(IdlTool, SerializeEnum) {
  WithEnum we;
  we.color = EColor::green;
  we.priority = EPriority::high;

  auto doc = parseRapid(we.serialize());
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["color"].GetString(), "green");
  EXPECT_STREQ(doc["priority"].GetString(), "high");
}

TEST(IdlTool, SerializeNested) {
  Outer o;
  o.label = "outer";
  o.child.value = "inner";
  o.child.count = 7;

  auto doc = parseRapid(o.serialize());
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["label"].GetString(), "outer");
  ASSERT_TRUE(doc["child"].IsObject());
  EXPECT_STREQ(doc["child"]["value"].GetString(), "inner");
  EXPECT_EQ(doc["child"]["count"].GetInt64(), 7);
}

TEST(IdlTool, SerializeOptionalPresent) {
  OptionalChildren oc;
  oc.name = "test";
  oc.first.emplace();
  oc.first->value = "a";
  oc.first->count = 1;

  auto doc = parseRapid(oc.serialize());
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.HasMember("first"));
  EXPECT_STREQ(doc["first"]["value"].GetString(), "a");
  EXPECT_FALSE(doc.HasMember("second"));
}

TEST(IdlTool, SerializeArray) {
  ArrayFields af;
  af.strings = {"x", "y"};
  af.numbers = {10, 20};
  af.doubles = {1.5};
  af.items.push_back(Inner{});
  af.items[0].value = "item";
  af.items[0].count = 3;

  auto doc = parseRapid(af.serialize());
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["strings"].IsArray());
  EXPECT_EQ(doc["strings"].Size(), 2u);
  EXPECT_STREQ(doc["strings"][0].GetString(), "x");
  ASSERT_TRUE(doc["items"].IsArray());
  EXPECT_EQ(doc["items"].Size(), 1u);
  EXPECT_STREQ(doc["items"][0]["value"].GetString(), "item");
}

TEST(IdlTool, SerializeOptionalArray) {
  OptionalArrays oa;
  oa.label = "test";
  oa.tags.emplace(std::vector<std::string>{"a", "b"});

  auto doc = parseRapid(oa.serialize());
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.HasMember("tags"));
  EXPECT_EQ(doc["tags"].Size(), 2u);
  EXPECT_FALSE(doc.HasMember("items"));
}

TEST(IdlTool, SerializeStringEscapes) {
  Inner inner;
  inner.value = "line1\nline2\ttab\"quote\\backslash";
  inner.count = 1;

  auto doc = parseRapid(inner.serialize());
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["value"].GetString(), "line1\nline2\ttab\"quote\\backslash");
}

// ============================================================================
// Roundtrip tests — parse → serialize → parse via rapidjson → compare
// ============================================================================

TEST(IdlTool, RoundtripCombined) {
  const char *json = R"({"id":"u1","name":"User","createdAt":1000,"updatedAt":2000,"priority":"high","active":true,"score":9.5,"metadata":{"value":"meta","count":3},"tags":["a","b"],"children":[{"value":"c1","count":1}],"nested":{"label":"n","child":{"value":"nc","count":2}}})";

  Combined c;
  ASSERT_TRUE(c.parse(json, strlen(json)));
  EXPECT_EQ(c.id, "u1");
  EXPECT_EQ(c.priority, EPriority::high);
  EXPECT_EQ(c.active, true);
  ASSERT_TRUE(c.metadata.has_value());
  EXPECT_EQ(c.metadata->value, "meta");
  ASSERT_TRUE(c.tags.has_value());
  EXPECT_EQ(c.tags->size(), 2u);
  EXPECT_EQ(c.children.size(), 1u);
  EXPECT_EQ(c.nested.label, "n");

  auto doc = parseRapid(c.serialize());
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["id"].GetString(), "u1");
  EXPECT_STREQ(doc["priority"].GetString(), "high");
  EXPECT_EQ(doc["active"].GetBool(), true);
  EXPECT_DOUBLE_EQ(doc["score"].GetDouble(), 9.5);
  ASSERT_TRUE(doc["metadata"].IsObject());
  EXPECT_STREQ(doc["metadata"]["value"].GetString(), "meta");
  ASSERT_TRUE(doc["tags"].IsArray());
  EXPECT_EQ(doc["tags"].Size(), 2u);
  ASSERT_TRUE(doc["children"].IsArray());
  EXPECT_EQ(doc["children"].Size(), 1u);
}

TEST(IdlTool, RoundtripScalars) {
  ScalarTypes original;
  original.fieldString = "roundtrip";
  original.fieldBool = true;
  original.fieldInt32 = -42;
  original.fieldUint32 = 999;
  original.fieldInt64 = -100000;
  original.fieldUint64 = 200000;
  original.fieldDouble = 1.23456;

  ScalarTypes parsed;
  auto json = original.serialize();
  ASSERT_TRUE(parsed.parse(json.data(), json.size()));
  EXPECT_EQ(parsed.fieldString, original.fieldString);
  EXPECT_EQ(parsed.fieldBool, original.fieldBool);
  EXPECT_EQ(parsed.fieldInt32, original.fieldInt32);
  EXPECT_EQ(parsed.fieldUint32, original.fieldUint32);
  EXPECT_EQ(parsed.fieldInt64, original.fieldInt64);
  EXPECT_EQ(parsed.fieldUint64, original.fieldUint64);
  EXPECT_DOUBLE_EQ(parsed.fieldDouble, original.fieldDouble);
}

// ============================================================================
// Diagnostic parse tests — verify error reporting (row, col, message)
// ============================================================================

TEST(IdlTool, DiagInvalidJson) {
  const char *json = "not json";
  ScalarDefaults d;
  ParseError error;
  EXPECT_FALSE(d.parse(json, strlen(json), error));
  EXPECT_EQ(error.row, 1);
  EXPECT_EQ(error.col, 1);
  EXPECT_NE(error.message.find("expected '{'"), std::string::npos);
}

TEST(IdlTool, DiagUnterminatedString) {
  const char *json = R"({"value":"hello)";
  Inner inner;
  ParseError error;
  EXPECT_FALSE(inner.parse(json, strlen(json), error));
  EXPECT_NE(error.message.find("field 'value'"), std::string::npos);
  EXPECT_NE(error.message.find("unterminated string"), std::string::npos);
}

TEST(IdlTool, DiagMissingColon) {
  const char *json = R"({"value" "x"})";
  Inner inner;
  ParseError error;
  EXPECT_FALSE(inner.parse(json, strlen(json), error));
  EXPECT_NE(error.message.find("expected ':'"), std::string::npos);
}

TEST(IdlTool, DiagMissingRequired) {
  const char *json = R"({"fieldString":"hi"})";
  ScalarTypes t;
  ParseError error;
  EXPECT_FALSE(t.parse(json, strlen(json), error));
  EXPECT_NE(error.message.find("missing required field"), std::string::npos);
}

TEST(IdlTool, DiagInvalidEnum) {
  const char *json = R"({"color":"pink"})";
  WithEnum we;
  ParseError error;
  EXPECT_FALSE(we.parse(json, strlen(json), error));
  EXPECT_NE(error.message.find("invalid enum value 'pink'"), std::string::npos);
}

TEST(IdlTool, DiagWrongTypeString) {
  const char *json = R"({"value":42,"count":1})";
  Inner inner;
  ParseError error;
  EXPECT_FALSE(inner.parse(json, strlen(json), error));
  EXPECT_NE(error.message.find("field 'value'"), std::string::npos);
  // Scanner reports "expected string, got '4'" — field context prepended
  EXPECT_NE(error.message.find("expected string"), std::string::npos);
}

TEST(IdlTool, DiagWrongTypeBool) {
  const char *json = R"({"fieldString":"x","fieldBool":"notbool","fieldInt32":1,"fieldUint32":1,"fieldInt64":1,"fieldUint64":1,"fieldDouble":1.0})";
  ScalarTypes t;
  ParseError error;
  EXPECT_FALSE(t.parse(json, strlen(json), error));
  EXPECT_NE(error.message.find("field 'fieldBool'"), std::string::npos);
  EXPECT_NE(error.message.find("expected boolean"), std::string::npos);
}

TEST(IdlTool, DiagNestedError) {
  const char *json = R"({"label":"test","child":{"value":42,"count":5}})";
  Outer o;
  ParseError error;
  EXPECT_FALSE(o.parse(json, strlen(json), error));
  // Error should be from the nested struct — "value" expects string but got 42
  EXPECT_NE(error.message.find("expected string"), std::string::npos);
}

TEST(IdlTool, DiagArrayElementError) {
  const char *json = R"({"strings":["a",42,"c"],"numbers":[],"doubles":[],"items":[]})";
  ArrayFields af;
  ParseError error;
  EXPECT_FALSE(af.parse(json, strlen(json), error));
  EXPECT_NE(error.message.find("expected string"), std::string::npos);
}

TEST(IdlTool, DiagMultilinePosition) {
  const char *json =
    "{\n"
    "  \"value\": \"x\",\n"
    "  \"count\": \"bad\"\n"
    "}";
  Inner inner;
  ParseError error;
  EXPECT_FALSE(inner.parse(json, strlen(json), error));
  EXPECT_EQ(error.row, 3);
  EXPECT_NE(error.message.find("field 'count'"), std::string::npos);
}

TEST(IdlTool, DiagValidJsonNoDiagError) {
  // Valid JSON should succeed with diag parse too
  const char *json = R"({"value":"x","count":1})";
  Inner inner;
  ParseError error;
  ASSERT_TRUE(inner.parse(json, strlen(json), error));
  EXPECT_TRUE(error.message.empty());
}
