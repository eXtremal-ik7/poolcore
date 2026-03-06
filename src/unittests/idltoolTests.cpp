#include "gtest/gtest.h"
#include "test.idl.h"
#include "p2putils/xmstream.h"
#include "rapidjson/document.h"

static rapidjson::Document parseRapid(const xmstream &stream) {
  rapidjson::Document doc;
  doc.Parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf());
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

TEST(IdlTool, ParseChronoFields) {
  const char *json = R"({"timeoutSec":15,"ttlHours":2})";
  ChronoFields f;
  ASSERT_TRUE(f.parse(json, strlen(json)));
  EXPECT_EQ(f.timeoutSec, std::chrono::seconds(15));
  EXPECT_EQ(f.retryMin, std::chrono::minutes(5));
  EXPECT_EQ(f.ttlHours, std::chrono::hours(2));
}

TEST(IdlTool, ParseMixedFields) {
  const char *json = R"({"required1":"x","required2":10,"required3":true})";
  MixedFields m;
  ASSERT_TRUE(m.parse(json, strlen(json)));
  EXPECT_EQ(m.required1, "x");
  EXPECT_EQ(m.required2, 10);
  EXPECT_EQ(m.required3, true);
  EXPECT_EQ(m.optional1, "");
  EXPECT_EQ(m.optional2, -1);
  EXPECT_EQ(m.optional3, false);
}

TEST(IdlTool, ParseMixedFieldsMissingRequired) {
  const char *json = R"({"required1":"x","required3":true})";
  MixedFields m;
  EXPECT_FALSE(m.parse(json, strlen(json)));
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

TEST(IdlTool, ParseEnumArray) {
  const char *json = R"({"priorities":["low","critical"]})";
  EnumArray ea;
  ASSERT_TRUE(ea.parse(json, strlen(json)));
  ASSERT_EQ(ea.priorities.size(), 2u);
  EXPECT_EQ(ea.priorities[0], EPriority::low);
  EXPECT_EQ(ea.priorities[1], EPriority::critical);
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

TEST(IdlTool, ParseNullableScalars) {
  const char *json1 = R"({"title":"a","note":"hello","numbers":[1,2,3]})";
  NullableScalars s1;
  ASSERT_TRUE(s1.parse(json1, strlen(json1)));
  EXPECT_EQ(s1.title, "a");
  ASSERT_TRUE(s1.note.has_value());
  EXPECT_EQ(*s1.note, "hello");
  ASSERT_TRUE(s1.numbers.has_value());
  ASSERT_EQ(s1.numbers->size(), 3u);
  EXPECT_EQ((*s1.numbers)[1], 2);

  const char *json2 = R"({"title":"b","note":null,"numbers":null})";
  NullableScalars s2;
  ASSERT_TRUE(s2.parse(json2, strlen(json2)));
  EXPECT_FALSE(s2.note.has_value());
  EXPECT_FALSE(s2.numbers.has_value());
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

TEST(IdlTool, ParseUnknownFieldRejected) {
  const char *json = R"({"value":"x","count":1,"extra":"bad"})";
  Inner inner;
  EXPECT_FALSE(inner.parse(json, strlen(json)));
}

TEST(IdlTool, ParseWithComments) {
  const char *json =
    "{\n"
    "  // inline comment\n"
    "  \"value\": \"x\", /* block comment */\n"
    "  \"count\": 1\n"
    "}";
  Inner inner;
  ASSERT_TRUE(inner.parse(json, strlen(json)));
  EXPECT_EQ(inner.value, "x");
  EXPECT_EQ(inner.count, 1);
}

TEST(IdlTool, ParseTrailingGarbageRejected) {
  const char *json = "{\"value\":\"x\",\"count\":1} trailing";
  Inner inner;
  EXPECT_FALSE(inner.parse(json, strlen(json)));
}

TEST(IdlTool, ParseInlineMapped) {
  const char *json = R"({"amount":"42","optionalAmount":"7","nullableAmount":null})";
  InlineMapped m;
  ASSERT_TRUE(m.parse(json, strlen(json)));
  EXPECT_EQ(m.amount, 42);
  ASSERT_TRUE(m.optionalAmount.has_value());
  EXPECT_EQ(*m.optionalAmount, 7);
  EXPECT_FALSE(m.nullableAmount.has_value());
}

TEST(IdlTool, ParseInlineMappedInvalid) {
  const char *json = R"({"amount":"nope"})";
  InlineMapped m;
  EXPECT_FALSE(m.parse(json, strlen(json)));
}

TEST(IdlTool, ParseInlineMappedArray) {
  const char *json = R"({"amounts":["1","2"],"optionalAmounts":["5"],"nullableAmounts":null})";
  InlineMappedArray m;
  ASSERT_TRUE(m.parse(json, strlen(json)));
  ASSERT_EQ(m.amounts.size(), 2u);
  EXPECT_EQ(m.amounts[0], 1);
  EXPECT_EQ(m.amounts[1], 2);
  ASSERT_TRUE(m.optionalAmounts.has_value());
  ASSERT_EQ(m.optionalAmounts->size(), 1u);
  EXPECT_EQ((*m.optionalAmounts)[0], 5);
  EXPECT_FALSE(m.nullableAmounts.has_value());
}

TEST(IdlTool, ParseInlineMappedArrayInvalid) {
  const char *json = R"({"amounts":["1","bad"]})";
  InlineMappedArray m;
  EXPECT_FALSE(m.parse(json, strlen(json)));
}

TEST(IdlTool, DiagUnknownField) {
  const char *json = R"({"value":"x","count":1,"extra":"bad"})";
  Inner inner;
  ParseError error;
  EXPECT_FALSE(inner.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("unknown field"), std::string::npos);
  EXPECT_NE(error.message.find("extra"), std::string::npos);
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

  xmstream stream;
  t.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["fieldString"].GetString(), "test");
  EXPECT_EQ(doc["fieldBool"].GetBool(), false);
  EXPECT_EQ(doc["fieldInt32"].GetInt(), -5);
  EXPECT_EQ(doc["fieldUint32"].GetUint(), 10u);
  EXPECT_EQ(doc["fieldInt64"].GetInt64(), 9999);
  EXPECT_EQ(doc["fieldUint64"].GetUint64(), 12345u);
  EXPECT_DOUBLE_EQ(doc["fieldDouble"].GetDouble(), 2.718);
}

TEST(IdlTool, SerializeChronoFields) {
  ChronoFields f;
  f.timeoutSec = std::chrono::seconds(30);
  f.retryMin = std::chrono::minutes(2);
  f.ttlHours = std::chrono::hours(8);

  xmstream stream;
  f.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_EQ(doc["timeoutSec"].GetInt64(), 30);
  EXPECT_EQ(doc["retryMin"].GetInt64(), 2);
  EXPECT_EQ(doc["ttlHours"].GetInt64(), 8);
}

TEST(IdlTool, SerializeMixedFields) {
  MixedFields m;
  m.required1 = "r1";
  m.required2 = 123;
  m.required3 = true;
  m.optional1 = "opt";
  m.optional2 = 456;
  m.optional3 = false;

  xmstream stream;
  m.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["required1"].GetString(), "r1");
  EXPECT_EQ(doc["required2"].GetInt64(), 123);
  EXPECT_EQ(doc["required3"].GetBool(), true);
  EXPECT_STREQ(doc["optional1"].GetString(), "opt");
  EXPECT_EQ(doc["optional2"].GetInt64(), 456);
  EXPECT_EQ(doc["optional3"].GetBool(), false);
}

TEST(IdlTool, SerializeEnum) {
  WithEnum we;
  we.color = EColor::green;
  we.priority = EPriority::high;

  xmstream stream;
  we.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["color"].GetString(), "green");
  EXPECT_STREQ(doc["priority"].GetString(), "high");
}

TEST(IdlTool, SerializeEnumArray) {
  EnumArray ea;
  ea.priorities = {EPriority::low, EPriority::critical};

  xmstream stream;
  ea.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["priorities"].IsArray());
  ASSERT_EQ(doc["priorities"].Size(), 2u);
  EXPECT_STREQ(doc["priorities"][0].GetString(), "low");
  EXPECT_STREQ(doc["priorities"][1].GetString(), "critical");
}

TEST(IdlTool, SerializeNested) {
  Outer o;
  o.label = "outer";
  o.child.value = "inner";
  o.child.count = 7;

  xmstream stream;
  o.serialize(stream);
  auto doc = parseRapid(stream);
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

  xmstream stream;
  oc.serialize(stream);
  auto doc = parseRapid(stream);
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

  xmstream stream;
  af.serialize(stream);
  auto doc = parseRapid(stream);
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

  xmstream stream;
  oa.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.HasMember("tags"));
  EXPECT_EQ(doc["tags"].Size(), 2u);
  EXPECT_FALSE(doc.HasMember("items"));
}

TEST(IdlTool, SerializeNullableScalars) {
  NullableScalars s;
  s.title = "x";

  xmstream stream1;
  s.serialize(stream1);
  auto doc1 = parseRapid(stream1);
  ASSERT_FALSE(doc1.HasParseError());
  EXPECT_STREQ(doc1["title"].GetString(), "x");
  ASSERT_TRUE(doc1.HasMember("note"));
  ASSERT_TRUE(doc1["note"].IsNull());
  ASSERT_TRUE(doc1.HasMember("numbers"));
  ASSERT_TRUE(doc1["numbers"].IsNull());

  s.note = "present";
  s.numbers = std::vector<int64_t>{4, 5};
  xmstream stream2;
  s.serialize(stream2);
  auto doc2 = parseRapid(stream2);
  ASSERT_FALSE(doc2.HasParseError());
  EXPECT_STREQ(doc2["note"].GetString(), "present");
  ASSERT_TRUE(doc2["numbers"].IsArray());
  EXPECT_EQ(doc2["numbers"].Size(), 2u);
  EXPECT_EQ(doc2["numbers"][0].GetInt64(), 4);
}

TEST(IdlTool, SerializeInlineMapped) {
  InlineMapped m;
  m.amount = 100;

  xmstream stream1;
  m.serialize(stream1);
  auto doc1 = parseRapid(stream1);
  ASSERT_FALSE(doc1.HasParseError());
  EXPECT_STREQ(doc1["amount"].GetString(), "100");
  EXPECT_FALSE(doc1.HasMember("optionalAmount"));
  ASSERT_TRUE(doc1.HasMember("nullableAmount"));
  ASSERT_TRUE(doc1["nullableAmount"].IsNull());

  m.optionalAmount = 5;
  m.nullableAmount = 7;
  xmstream stream2;
  m.serialize(stream2);
  auto doc2 = parseRapid(stream2);
  ASSERT_FALSE(doc2.HasParseError());
  EXPECT_STREQ(doc2["optionalAmount"].GetString(), "5");
  EXPECT_STREQ(doc2["nullableAmount"].GetString(), "7");
}

TEST(IdlTool, SerializeInlineMappedArray) {
  InlineMappedArray m;
  m.amounts = {1, 2};

  xmstream stream1;
  m.serialize(stream1);
  auto doc1 = parseRapid(stream1);
  ASSERT_FALSE(doc1.HasParseError());
  ASSERT_TRUE(doc1["amounts"].IsArray());
  ASSERT_EQ(doc1["amounts"].Size(), 2u);
  EXPECT_STREQ(doc1["amounts"][0].GetString(), "1");
  EXPECT_STREQ(doc1["amounts"][1].GetString(), "2");
  EXPECT_FALSE(doc1.HasMember("optionalAmounts"));
  ASSERT_TRUE(doc1.HasMember("nullableAmounts"));
  ASSERT_TRUE(doc1["nullableAmounts"].IsNull());

  m.optionalAmounts = std::vector<int64_t>{3};
  m.nullableAmounts = std::vector<int64_t>{4, 5};
  xmstream stream2;
  m.serialize(stream2);
  auto doc2 = parseRapid(stream2);
  ASSERT_FALSE(doc2.HasParseError());
  ASSERT_TRUE(doc2["optionalAmounts"].IsArray());
  EXPECT_STREQ(doc2["optionalAmounts"][0].GetString(), "3");
  ASSERT_TRUE(doc2["nullableAmounts"].IsArray());
  EXPECT_STREQ(doc2["nullableAmounts"][1].GetString(), "5");
}

TEST(IdlTool, SerializeStringEscapes) {
  Inner inner;
  inner.value = "line1\nline2\ttab\"quote\\backslash";
  inner.count = 1;

  xmstream stream;
  inner.serialize(stream);
  auto doc = parseRapid(stream);
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

  xmstream stream;
  c.serialize(stream);
  auto doc = parseRapid(stream);
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

  xmstream stream;
  original.serialize(stream);
  ScalarTypes parsed;
  ASSERT_TRUE(parsed.parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf()));
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
  EXPECT_FALSE(d.parseVerbose(json, strlen(json), error));
  EXPECT_EQ(error.row, 1);
  EXPECT_EQ(error.col, 1);
  EXPECT_NE(error.message.find("expected '{'"), std::string::npos);
}

TEST(IdlTool, DiagUnterminatedString) {
  const char *json = R"({"value":"hello)";
  Inner inner;
  ParseError error;
  EXPECT_FALSE(inner.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("field 'value'"), std::string::npos);
  EXPECT_NE(error.message.find("unterminated string"), std::string::npos);
}

TEST(IdlTool, DiagMissingColon) {
  const char *json = R"({"value" "x"})";
  Inner inner;
  ParseError error;
  EXPECT_FALSE(inner.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("expected ':'"), std::string::npos);
}

TEST(IdlTool, DiagMissingRequired) {
  const char *json = R"({"fieldString":"hi"})";
  ScalarTypes t;
  ParseError error;
  EXPECT_FALSE(t.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("missing required field"), std::string::npos);
}

TEST(IdlTool, DiagChronoWrongType) {
  const char *json = R"({"timeoutSec":"x","ttlHours":1})";
  ChronoFields f;
  ParseError error;
  EXPECT_FALSE(f.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("field 'timeoutSec'"), std::string::npos);
  EXPECT_NE(error.message.find("expected integer"), std::string::npos);
}

TEST(IdlTool, DiagInlineMappedInvalid) {
  const char *json = R"({"amount":"nope"})";
  InlineMapped m;
  ParseError error;
  EXPECT_FALSE(m.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("field 'amount'"), std::string::npos);
  EXPECT_NE(error.message.find("invalid mapped value"), std::string::npos);
}

TEST(IdlTool, DiagInlineMappedValidNoDiagError) {
  const char *json = R"({"amount":"42"})";
  InlineMapped m;
  ParseError error;
  ASSERT_TRUE(m.parseVerbose(json, strlen(json), error));
  EXPECT_TRUE(error.message.empty());
}

TEST(IdlTool, DiagInvalidEnum) {
  const char *json = R"({"color":"pink"})";
  WithEnum we;
  ParseError error;
  EXPECT_FALSE(we.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("invalid enum value 'pink'"), std::string::npos);
}

TEST(IdlTool, DiagWrongTypeString) {
  const char *json = R"({"value":42,"count":1})";
  Inner inner;
  ParseError error;
  EXPECT_FALSE(inner.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("field 'value'"), std::string::npos);
  // Scanner reports "expected string, got '4'" — field context prepended
  EXPECT_NE(error.message.find("expected string"), std::string::npos);
}

TEST(IdlTool, DiagWrongTypeBool) {
  const char *json = R"({"fieldString":"x","fieldBool":"notbool","fieldInt32":1,"fieldUint32":1,"fieldInt64":1,"fieldUint64":1,"fieldDouble":1.0})";
  ScalarTypes t;
  ParseError error;
  EXPECT_FALSE(t.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("field 'fieldBool'"), std::string::npos);
  EXPECT_NE(error.message.find("expected boolean"), std::string::npos);
}

TEST(IdlTool, DiagNestedError) {
  const char *json = R"({"label":"test","child":{"value":42,"count":5}})";
  Outer o;
  ParseError error;
  EXPECT_FALSE(o.parseVerbose(json, strlen(json), error));
  // Error should be from the nested struct — "value" expects string but got 42
  EXPECT_NE(error.message.find("expected string"), std::string::npos);
}

TEST(IdlTool, DiagArrayElementError) {
  const char *json = R"({"strings":["a",42,"c"],"numbers":[],"doubles":[],"items":[]})";
  ArrayFields af;
  ParseError error;
  EXPECT_FALSE(af.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("expected string"), std::string::npos);
}

TEST(IdlTool, DiagEnumArrayElementError) {
  const char *json = R"({"priorities":["low","bad"]})";
  EnumArray ea;
  ParseError error;
  EXPECT_FALSE(ea.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("invalid enum value"), std::string::npos);
}

TEST(IdlTool, DiagInlineMappedArrayElementError) {
  const char *json = R"({"amounts":["1","bad"]})";
  InlineMappedArray m;
  ParseError error;
  EXPECT_FALSE(m.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("field 'amounts'"), std::string::npos);
  EXPECT_NE(error.message.find("invalid mapped value in array"), std::string::npos);
}

TEST(IdlTool, DiagMultilinePosition) {
  const char *json =
    "{\n"
    "  \"value\": \"x\",\n"
    "  \"count\": \"bad\"\n"
    "}";
  Inner inner;
  ParseError error;
  EXPECT_FALSE(inner.parseVerbose(json, strlen(json), error));
  EXPECT_EQ(error.row, 3);
  EXPECT_NE(error.message.find("field 'count'"), std::string::npos);
}

TEST(IdlTool, DiagValidJsonNoDiagError) {
  // Valid JSON should succeed with diag parse too
  const char *json = R"({"value":"x","count":1})";
  Inner inner;
  ParseError error;
  ASSERT_TRUE(inner.parseVerbose(json, strlen(json), error));
  EXPECT_TRUE(error.message.empty());
}

TEST(IdlTool, DiagTrailingGarbage) {
  const char *json = "{\"value\":\"x\",\"count\":1} trailing";
  Inner inner;
  ParseError error;
  EXPECT_FALSE(inner.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("trailing characters"), std::string::npos);
}

// ============================================================================
// Context vector tests
// ============================================================================

TEST(IdlTool, ContextVectorSerialize) {
  // CtxChild has context testVal (uint32 context).
  // CtxVectorParent has items: [CtxChild] → serialize takes vector<uint32_t>.
  CtxVectorParent parent;
  parent.items.resize(3);
  parent.items[0].value = 12345;
  parent.items[0].label = "a";
  parent.items[1].value = 67890;
  parent.items[1].label = "b";
  parent.items[2].value = 100;
  parent.items[2].label = "c";

  // ctx[0]=2 → 123.45, ctx[1]=3 → 67.890, ctx[2]=0 → 100
  std::vector<uint32_t> ctx = {2, 3, 0};

  xmstream stream;
  parent.serialize(stream, ctx);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["items"].IsArray());
  ASSERT_EQ(doc["items"].Size(), 3u);
  // Element 0: value=12345 with ctx=2 → "123.45"
  EXPECT_STREQ(doc["items"][0]["value"].GetString(), "123.45");
  EXPECT_STREQ(doc["items"][0]["label"].GetString(), "a");
  // Element 1: value=67890 with ctx=3 → "67.890"
  EXPECT_STREQ(doc["items"][1]["value"].GetString(), "67.890");
  // Element 2: value=100 with ctx=0 → "100"
  EXPECT_STREQ(doc["items"][2]["value"].GetString(), "100");
}

TEST(IdlTool, ContextVectorResolve) {
  // Parse JSON with string values, then resolve with per-element context
  const char *json = R"({"items":[{"value":"123.45","label":"a"},{"value":"67.890","label":"b"},{"value":"100","label":"c"}]})";

  CtxVectorParent parent;
  CtxVectorParent::Capture capture;
  ASSERT_TRUE(parent.parse(json, strlen(json), capture));
  EXPECT_EQ(parent.items.size(), 3u);

  std::vector<uint32_t> ctx = {2, 3, 0};
  ASSERT_TRUE(CtxVectorParent::resolve(parent, capture, ctx));

  // Element 0: "123.45" with ctx=2 → 12345
  EXPECT_EQ(parent.items[0].value, 12345);
  EXPECT_EQ(parent.items[0].label, "a");
  // Element 1: "67.890" with ctx=3 → 67890
  EXPECT_EQ(parent.items[1].value, 67890);
  // Element 2: "100" with ctx=0 → 100
  EXPECT_EQ(parent.items[2].value, 100);
}

// ============================================================================
// Flat serialize tests
// ============================================================================

// ============================================================================
// Bug-exposing tests (issues 1–4)
// ============================================================================

// Issue 1: getWireTypeInfo returns jsonWriteInt for uint64 wire type.
// Large uint64 values (> INT64_MAX) get written as negative numbers.
TEST(IdlTool, BugUint64WireSerialize) {
  WithUint64Mapped m;
  m.counter = 10000000000000000000ULL;  // > INT64_MAX

  xmstream stream;
  m.serialize(stream);
  std::string json(reinterpret_cast<const char*>(stream.data()), stream.sizeOf());

  // Bug: jsonWriteInt formats uint64 as signed → negative number in output
  EXPECT_EQ(json.find('-'), std::string::npos) << "Large uint64 serialized as negative: " << json;
}

// Issue 2: \uXXXX escape handling drops hex digits, produces literal '\u'
// instead of decoding the Unicode codepoint.
TEST(IdlTool, BugUnicodeEscape) {
  const char *json = R"({"value":"hello\u0041world","count":1})";
  Inner inner;
  ASSERT_TRUE(inner.parse(json, strlen(json)));
  // \u0041 = 'A', should be decoded
  EXPECT_EQ(inner.value, "helloAworld");
}

// Issue 3: readInt32/readUInt32 silently truncate values out of range
// instead of reporting an error.
TEST(IdlTool, BugInt32Overflow) {
  // 3000000000 > INT32_MAX (2147483647)
  const char *json = R"({"fieldString":"x","fieldBool":true,"fieldInt32":3000000000,"fieldUint32":1,"fieldInt64":1,"fieldUint64":1,"fieldDouble":1.0})";
  ScalarTypes t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(IdlTool, BugUint32Overflow) {
  // 5000000000 > UINT32_MAX (4294967295)
  const char *json = R"({"fieldString":"x","fieldBool":true,"fieldInt32":1,"fieldUint32":5000000000,"fieldInt64":1,"fieldUint64":1,"fieldDouble":1.0})";
  ScalarTypes t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

// Issue 4: chrono types in arrays — codegen produces push_back(int64_t) into
// vector<chrono::seconds>, which won't compile (explicit constructor).
// If this test compiles, the bug is fixed.
TEST(IdlTool, BugChronoArrayParse) {
  const char *json = R"({"durations":[10,20,30]})";
  ChronoArray ca;
  ASSERT_TRUE(ca.parse(json, strlen(json)));
  ASSERT_EQ(ca.durations.size(), 3u);
  EXPECT_EQ(ca.durations[0], std::chrono::seconds(10));
  EXPECT_EQ(ca.durations[1], std::chrono::seconds(20));
  EXPECT_EQ(ca.durations[2], std::chrono::seconds(30));
}

TEST(IdlTool, BugChronoArraySerialize) {
  ChronoArray ca;
  ca.durations = {std::chrono::seconds(5), std::chrono::seconds(15)};

  xmstream stream;
  ca.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["durations"].IsArray());
  ASSERT_EQ(doc["durations"].Size(), 2u);
  EXPECT_EQ(doc["durations"][0].GetInt64(), 5);
  EXPECT_EQ(doc["durations"][1].GetInt64(), 15);
}

TEST(IdlTool, BugChronoArrayVerboseParse) {
  const char *json = R"({"durations":[10,20]})";
  ChronoArray ca;
  ParseError error;
  ASSERT_TRUE(ca.parseVerbose(json, strlen(json), error));
  ASSERT_EQ(ca.durations.size(), 2u);
  EXPECT_EQ(ca.durations[0], std::chrono::seconds(10));
  EXPECT_EQ(ca.durations[1], std::chrono::seconds(20));
}

// ============================================================================
// Coverage gap tests
// ============================================================================

// --- Nullable struct object (Inner??) ---

TEST(IdlTool, ParseNullableObjectPresent) {
  const char *json = R"({"title":"t","inner":{"value":"v","count":1}})";
  NullableObject no;
  ASSERT_TRUE(no.parse(json, strlen(json)));
  EXPECT_EQ(no.title, "t");
  ASSERT_TRUE(no.inner.has_value());
  EXPECT_EQ(no.inner->value, "v");
  EXPECT_EQ(no.inner->count, 1);
}

TEST(IdlTool, ParseNullableObjectNull) {
  const char *json = R"({"title":"t","inner":null})";
  NullableObject no;
  ASSERT_TRUE(no.parse(json, strlen(json)));
  EXPECT_FALSE(no.inner.has_value());
}

TEST(IdlTool, ParseNullableObjectAbsent) {
  // NullableObject (Inner??) — absent field should still succeed (default nullopt)
  const char *json = R"({"title":"t"})";
  NullableObject no;
  ASSERT_TRUE(no.parse(json, strlen(json)));
  EXPECT_FALSE(no.inner.has_value());
}

TEST(IdlTool, SerializeNullableObjectPresent) {
  NullableObject no;
  no.title = "t";
  no.inner.emplace();
  no.inner->value = "v";
  no.inner->count = 42;

  xmstream stream;
  no.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["title"].GetString(), "t");
  ASSERT_TRUE(doc.HasMember("inner"));
  ASSERT_TRUE(doc["inner"].IsObject());
  EXPECT_STREQ(doc["inner"]["value"].GetString(), "v");
  EXPECT_EQ(doc["inner"]["count"].GetInt64(), 42);
}

TEST(IdlTool, SerializeNullableObjectAbsent) {
  NullableObject no;
  no.title = "t";
  // inner is nullopt

  xmstream stream;
  no.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  // Nullable emits null (unlike optional which omits)
  ASSERT_TRUE(doc.HasMember("inner"));
  EXPECT_TRUE(doc["inner"].IsNull());
}

TEST(IdlTool, VerboseParseNullableObjectPresent) {
  const char *json = R"({"title":"t","inner":{"value":"v","count":1}})";
  NullableObject no;
  ParseError error;
  ASSERT_TRUE(no.parseVerbose(json, strlen(json), error));
  ASSERT_TRUE(no.inner.has_value());
  EXPECT_EQ(no.inner->value, "v");
}

TEST(IdlTool, VerboseParseNullableObjectNull) {
  const char *json = R"({"title":"t","inner":null})";
  NullableObject no;
  ParseError error;
  ASSERT_TRUE(no.parseVerbose(json, strlen(json), error));
  EXPECT_FALSE(no.inner.has_value());
}

TEST(IdlTool, RoundtripNullableObject) {
  NullableObject original;
  original.title = "roundtrip";
  original.inner.emplace();
  original.inner->value = "inner";
  original.inner->count = 99;

  xmstream stream;
  original.serialize(stream);
  NullableObject parsed;
  ASSERT_TRUE(parsed.parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf()));
  EXPECT_EQ(parsed.title, original.title);
  ASSERT_TRUE(parsed.inner.has_value());
  EXPECT_EQ(parsed.inner->value, original.inner->value);
  EXPECT_EQ(parsed.inner->count, original.inner->count);
}

// --- Empty arrays ---

TEST(IdlTool, ParseEmptyArrays) {
  const char *json = R"({"strings":[],"numbers":[],"doubles":[],"items":[]})";
  ArrayFields af;
  ASSERT_TRUE(af.parse(json, strlen(json)));
  EXPECT_TRUE(af.strings.empty());
  EXPECT_TRUE(af.numbers.empty());
  EXPECT_TRUE(af.doubles.empty());
  EXPECT_TRUE(af.items.empty());
}

TEST(IdlTool, VerboseParseEmptyArrays) {
  const char *json = R"({"strings":[],"numbers":[],"doubles":[],"items":[]})";
  ArrayFields af;
  ParseError error;
  ASSERT_TRUE(af.parseVerbose(json, strlen(json), error));
  EXPECT_TRUE(af.strings.empty());
  EXPECT_TRUE(af.numbers.empty());
  EXPECT_TRUE(af.items.empty());
}

TEST(IdlTool, SerializeEmptyArrays) {
  ArrayFields af;
  xmstream stream;
  af.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["strings"].IsArray());
  EXPECT_EQ(doc["strings"].Size(), 0u);
  ASSERT_TRUE(doc["numbers"].IsArray());
  EXPECT_EQ(doc["numbers"].Size(), 0u);
}

// --- Numeric edge cases ---

TEST(IdlTool, ParseInt64Boundaries) {
  // INT64_MAX = 9223372036854775807
  const char *jsonMax = R"({"fieldString":"x","fieldBool":true,"fieldInt32":1,"fieldUint32":1,"fieldInt64":9223372036854775807,"fieldUint64":1,"fieldDouble":1.0})";
  ScalarTypes tMax;
  ASSERT_TRUE(tMax.parse(jsonMax, strlen(jsonMax)));
  EXPECT_EQ(tMax.fieldInt64, INT64_MAX);

  // INT64_MIN = -9223372036854775808
  const char *jsonMin = R"({"fieldString":"x","fieldBool":true,"fieldInt32":1,"fieldUint32":1,"fieldInt64":-9223372036854775808,"fieldUint64":1,"fieldDouble":1.0})";
  ScalarTypes tMin;
  ASSERT_TRUE(tMin.parse(jsonMin, strlen(jsonMin)));
  EXPECT_EQ(tMin.fieldInt64, INT64_MIN);
}

TEST(IdlTool, ParseUint64Max) {
  // UINT64_MAX = 18446744073709551615
  const char *json = R"({"fieldString":"x","fieldBool":true,"fieldInt32":1,"fieldUint32":1,"fieldInt64":1,"fieldUint64":18446744073709551615,"fieldDouble":1.0})";
  ScalarTypes t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.fieldUint64, UINT64_MAX);
}

TEST(IdlTool, ParseNegativeUint64Rejected) {
  const char *json = R"({"fieldString":"x","fieldBool":true,"fieldInt32":1,"fieldUint32":1,"fieldInt64":1,"fieldUint64":-1,"fieldDouble":1.0})";
  ScalarTypes t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(IdlTool, ParseInt32Boundaries) {
  // INT32_MAX = 2147483647
  const char *jsonMax = R"({"fieldString":"x","fieldBool":true,"fieldInt32":2147483647,"fieldUint32":1,"fieldInt64":1,"fieldUint64":1,"fieldDouble":1.0})";
  ScalarTypes tMax;
  ASSERT_TRUE(tMax.parse(jsonMax, strlen(jsonMax)));
  EXPECT_EQ(tMax.fieldInt32, INT32_MAX);

  // INT32_MIN = -2147483648
  const char *jsonMin = R"({"fieldString":"x","fieldBool":true,"fieldInt32":-2147483648,"fieldUint32":1,"fieldInt64":1,"fieldUint64":1,"fieldDouble":1.0})";
  ScalarTypes tMin;
  ASSERT_TRUE(tMin.parse(jsonMin, strlen(jsonMin)));
  EXPECT_EQ(tMin.fieldInt32, INT32_MIN);
}

TEST(IdlTool, ParseUint32Max) {
  // UINT32_MAX = 4294967295
  const char *json = R"({"fieldString":"x","fieldBool":true,"fieldInt32":1,"fieldUint32":4294967295,"fieldInt64":1,"fieldUint64":1,"fieldDouble":1.0})";
  ScalarTypes t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.fieldUint32, UINT32_MAX);
}

// --- Control character serialization ---

TEST(IdlTool, SerializeControlChars) {
  Inner inner;
  inner.value = std::string("a\b" "b\fc\rd", 7);  // \b, \f, \r
  inner.count = 1;

  xmstream stream;
  inner.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  std::string parsed = doc["value"].GetString();
  EXPECT_EQ(parsed.size(), 7u);
  EXPECT_EQ(parsed[1], '\b');
  EXPECT_EQ(parsed[3], '\f');
  EXPECT_EQ(parsed[5], '\r');
}

TEST(IdlTool, SerializeNullChar) {
  Inner inner;
  inner.value = std::string("ab\0cd", 5);  // embedded null
  inner.count = 1;

  xmstream stream;
  inner.serialize(stream);
  // At least it should produce valid JSON (rapidjson may truncate at \0)
  std::string json(reinterpret_cast<const char*>(stream.data()), stream.sizeOf());
  EXPECT_NE(json.find("\\u0000"), std::string::npos);
}

// --- Whitespace variants ---

TEST(IdlTool, ParseWhitespaceVariants) {
  // Tabs, \r\n between fields
  const char *json = "{\r\n\t\"value\"\t:\t\"x\"\t,\r\n\t\"count\"\t:\t1\r\n}";
  Inner inner;
  ASSERT_TRUE(inner.parse(json, strlen(json)));
  EXPECT_EQ(inner.value, "x");
  EXPECT_EQ(inner.count, 1);
}

TEST(IdlTool, VerboseParseWhitespaceVariants) {
  const char *json = "{\r\n\t\"value\"\t:\t\"x\"\t,\r\n\t\"count\"\t:\t1\r\n}";
  Inner inner;
  ParseError error;
  ASSERT_TRUE(inner.parseVerbose(json, strlen(json), error));
  EXPECT_EQ(inner.value, "x");
}

// --- Verbose parse for optional/nullable types ---

TEST(IdlTool, VerboseParseOptionalObjectPresent) {
  const char *json = R"({"name":"x","first":{"value":"a","count":1}})";
  OptionalChildren oc;
  ParseError error;
  ASSERT_TRUE(oc.parseVerbose(json, strlen(json), error));
  ASSERT_TRUE(oc.first.has_value());
  EXPECT_EQ(oc.first->value, "a");
  EXPECT_FALSE(oc.second.has_value());
}

TEST(IdlTool, VerboseParseOptionalObjectNull) {
  const char *json = R"({"name":"x","first":null,"second":null})";
  OptionalChildren oc;
  ParseError error;
  ASSERT_TRUE(oc.parseVerbose(json, strlen(json), error));
  EXPECT_FALSE(oc.first.has_value());
  EXPECT_FALSE(oc.second.has_value());
}

TEST(IdlTool, VerboseParseOptionalArrayPresent) {
  const char *json = R"({"label":"x","tags":["a","b"]})";
  OptionalArrays oa;
  ParseError error;
  ASSERT_TRUE(oa.parseVerbose(json, strlen(json), error));
  ASSERT_TRUE(oa.tags.has_value());
  EXPECT_EQ(oa.tags->size(), 2u);
  EXPECT_FALSE(oa.items.has_value());
}

TEST(IdlTool, VerboseParseOptionalArrayNull) {
  const char *json = R"({"label":"x","tags":null})";
  OptionalArrays oa;
  ParseError error;
  ASSERT_TRUE(oa.parseVerbose(json, strlen(json), error));
  EXPECT_FALSE(oa.tags.has_value());
}

TEST(IdlTool, VerboseParseNullableScalarsPresent) {
  const char *json = R"({"title":"t","note":"hello","numbers":[1,2]})";
  NullableScalars ns;
  ParseError error;
  ASSERT_TRUE(ns.parseVerbose(json, strlen(json), error));
  ASSERT_TRUE(ns.note.has_value());
  EXPECT_EQ(*ns.note, "hello");
  ASSERT_TRUE(ns.numbers.has_value());
  EXPECT_EQ(ns.numbers->size(), 2u);
}

TEST(IdlTool, VerboseParseNullableScalarsNull) {
  const char *json = R"({"title":"t","note":null,"numbers":null})";
  NullableScalars ns;
  ParseError error;
  ASSERT_TRUE(ns.parseVerbose(json, strlen(json), error));
  EXPECT_FALSE(ns.note.has_value());
  EXPECT_FALSE(ns.numbers.has_value());
}

// --- Large enum (perfect hash path) ---

TEST(IdlTool, ParseLargeEnum) {
  const char *json = R"({"status":"completed"})";
  WithStatus ws;
  ASSERT_TRUE(ws.parse(json, strlen(json)));
  EXPECT_EQ(ws.status, EStatus::completed);
}

TEST(IdlTool, ParseLargeEnumAllValues) {
  // Test all 6 values to exercise the perfect hash
  const char *values[] = {"pending", "active", "paused", "completed", "cancelled", "failed"};
  EStatus expected[] = {EStatus::pending, EStatus::active, EStatus::paused,
                        EStatus::completed, EStatus::cancelled, EStatus::failed};

  for (int i = 0; i < 6; i++) {
    std::string json = std::format(R"({{"status":"{}"}})", values[i]);
    WithStatus ws;
    ASSERT_TRUE(ws.parse(json.c_str(), json.size())) << "Failed to parse: " << values[i];
    EXPECT_EQ(ws.status, expected[i]) << "Wrong enum for: " << values[i];
  }
}

TEST(IdlTool, ParseLargeEnumInvalid) {
  const char *json = R"({"status":"unknown"})";
  WithStatus ws;
  EXPECT_FALSE(ws.parse(json, strlen(json)));
}

TEST(IdlTool, VerboseParseLargeEnumInvalid) {
  const char *json = R"({"status":"unknown"})";
  WithStatus ws;
  ParseError error;
  EXPECT_FALSE(ws.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("invalid enum value"), std::string::npos);
}

TEST(IdlTool, SerializeLargeEnum) {
  WithStatus ws;
  ws.status = EStatus::cancelled;
  ws.label = "test";

  xmstream stream;
  ws.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["status"].GetString(), "cancelled");
}

TEST(IdlTool, RoundtripLargeEnum) {
  WithStatus original;
  original.status = EStatus::failed;
  original.label = "rt";

  xmstream stream;
  original.serialize(stream);
  WithStatus parsed;
  ASSERT_TRUE(parsed.parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf()));
  EXPECT_EQ(parsed.status, original.status);
  EXPECT_EQ(parsed.label, original.label);
}

// --- Scalar context (non-vector) ---

TEST(IdlTool, ContextScalarSerialize) {
  CtxScalarParent parent;
  parent.child.value = 12345;
  parent.child.label = "item";

  uint32_t ctx = 2;  // scale = 100 → "123.45"

  xmstream stream;
  parent.serialize(stream, ctx);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["child"].IsObject());
  EXPECT_STREQ(doc["child"]["value"].GetString(), "123.45");
  EXPECT_STREQ(doc["child"]["label"].GetString(), "item");
}

TEST(IdlTool, ContextScalarResolve) {
  const char *json = R"({"child":{"value":"123.45","label":"item"}})";

  CtxScalarParent parent;
  CtxScalarParent::Capture capture;
  ASSERT_TRUE(parent.parse(json, strlen(json), capture));

  uint32_t ctx = 2;
  ASSERT_TRUE(CtxScalarParent::resolve(parent, capture, ctx));
  EXPECT_EQ(parent.child.value, 12345);
  EXPECT_EQ(parent.child.label, "item");
}

// --- Roundtrip optional/nullable ---

TEST(IdlTool, RoundtripOptionalChildren) {
  OptionalChildren original;
  original.name = "test";
  original.first.emplace();
  original.first->value = "first";
  original.first->count = 10;
  // second is absent (nullopt)

  xmstream stream;
  original.serialize(stream);
  OptionalChildren parsed;
  ASSERT_TRUE(parsed.parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf()));
  EXPECT_EQ(parsed.name, "test");
  ASSERT_TRUE(parsed.first.has_value());
  EXPECT_EQ(parsed.first->value, "first");
  EXPECT_EQ(parsed.first->count, 10);
  EXPECT_FALSE(parsed.second.has_value());
}

TEST(IdlTool, RoundtripNullableArrays) {
  NullableScalars original;
  original.title = "test";
  original.note = "hello";
  original.numbers = std::vector<int64_t>{10, 20, 30};

  xmstream stream;
  original.serialize(stream);
  NullableScalars parsed;
  ASSERT_TRUE(parsed.parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf()));
  EXPECT_EQ(parsed.title, "test");
  ASSERT_TRUE(parsed.note.has_value());
  EXPECT_EQ(*parsed.note, "hello");
  ASSERT_TRUE(parsed.numbers.has_value());
  ASSERT_EQ(parsed.numbers->size(), 3u);
  EXPECT_EQ((*parsed.numbers)[2], 30);
}

// --- Serialize absent optional object (focused test) ---

TEST(IdlTool, SerializeAbsentOptionalObject) {
  OptionalChildren oc;
  oc.name = "empty";
  // both first and second are nullopt

  xmstream stream;
  oc.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["name"].GetString(), "empty");
  // Optional objects (?) should NOT appear in output when absent
  EXPECT_FALSE(doc.HasMember("first"));
  EXPECT_FALSE(doc.HasMember("second"));
}

// ============================================================================
// Flat serialize tests
// ============================================================================

TEST(IdlTool, FlatSerialize) {
  xmstream stream;
  std::vector<Inner> items;
  items.push_back(Inner{});
  items.back().value = "hello";
  items.back().count = 42;
  FlatResponse::serialize(stream, std::string("ok"), items);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["status"].GetString(), "ok");
  ASSERT_TRUE(doc["items"].IsArray());
  ASSERT_EQ(doc["items"].Size(), 1u);
  EXPECT_STREQ(doc["items"][0]["value"].GetString(), "hello");
  EXPECT_EQ(doc["items"][0]["count"].GetInt64(), 42);
}
