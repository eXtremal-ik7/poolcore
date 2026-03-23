#include "gtest/gtest.h"
#include "test.idl.h"
#include "p2putils/xmstream.h"
#include "rapidjson/document.h"
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

static rapidjson::Document parseRapid(const xmstream &stream) {
  rapidjson::Document doc;
  doc.Parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf());
  return doc;
}

// ============================================================================
// Enum array tests
// ============================================================================

TEST(IdlTool, ParseEnumArray) {
  const char *json = R"({"priorities":["low","critical"]})";
  EnumArray ea;
  ASSERT_TRUE(ea.parse(json, strlen(json)));
  ASSERT_EQ(ea.priorities.size(), 2u);
  EXPECT_EQ(ea.priorities[0], EPriority::low);
  EXPECT_EQ(ea.priorities[1], EPriority::critical);
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

TEST(IdlTool, DiagEnumArrayElementError) {
  const char *json = R"({"priorities":["low","bad"]})";
  EnumArray ea;
  ParseError error;
  EXPECT_FALSE(ea.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("invalid enum value"), std::string::npos);
}

// ============================================================================
// Array field tests
// ============================================================================

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

TEST(IdlTool, DiagArrayElementError) {
  const char *json = R"({"strings":["a",42,"c"],"numbers":[],"doubles":[],"items":[]})";
  ArrayFields af;
  ParseError error;
  EXPECT_FALSE(af.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("expected string"), std::string::npos);
}

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

// ============================================================================
// Optional array tests
// ============================================================================

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
  EXPECT_FALSE(oa2.parse(json2, strlen(json2)));

  const char *json3 = R"({"label":"z"})";
  OptionalArrays oa3;
  ASSERT_TRUE(oa3.parse(json3, strlen(json3)));
  EXPECT_FALSE(oa3.tags.has_value());
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
  EXPECT_FALSE(oa.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("null is not allowed"), std::string::npos);
}

// ============================================================================
// Inline mapped array tests
// ============================================================================

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

TEST(IdlTool, ParseInlineMappedArrayOptionalNullRejected) {
  const char *json = R"({"amounts":["1"],"optionalAmounts":null})";
  InlineMappedArray m;
  EXPECT_FALSE(m.parse(json, strlen(json)));
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

TEST(IdlTool, DiagInlineMappedArrayElementError) {
  const char *json = R"({"amounts":["1","bad"]})";
  InlineMappedArray m;
  ParseError error;
  EXPECT_FALSE(m.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("field 'amounts'"), std::string::npos);
  EXPECT_NE(error.message.find("invalid plainVal value in array"), std::string::npos);
}

// ============================================================================
// Chrono array tests
// ============================================================================

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
// Nullable arrays (NullableScalars)
// ============================================================================

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

// ============================================================================
// Fixed-size array tests
// ============================================================================

TEST(IdlTool, ParseFixedArrayScalar) {
  const char *json = R"({"coords":[1.0,2.0,3.0],"flags":[true,false]})";
  FixedArrayScalar t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_DOUBLE_EQ(t.coords[0], 1.0);
  EXPECT_DOUBLE_EQ(t.coords[1], 2.0);
  EXPECT_DOUBLE_EQ(t.coords[2], 3.0);
  EXPECT_EQ(t.flags[0], true);
  EXPECT_EQ(t.flags[1], false);
}

TEST(IdlTool, SerializeFixedArrayScalar) {
  FixedArrayScalar t;
  t.coords = {1.5, 2.5, 3.5};
  t.flags = {false, true};
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["coords"].IsArray());
  ASSERT_EQ(doc["coords"].Size(), 3u);
  EXPECT_DOUBLE_EQ(doc["coords"][0].GetDouble(), 1.5);
  EXPECT_DOUBLE_EQ(doc["coords"][1].GetDouble(), 2.5);
  EXPECT_DOUBLE_EQ(doc["coords"][2].GetDouble(), 3.5);
  ASSERT_TRUE(doc["flags"].IsArray());
  EXPECT_EQ(doc["flags"][0].GetBool(), false);
  EXPECT_EQ(doc["flags"][1].GetBool(), true);
}

TEST(IdlTool, RoundtripFixedArrayScalar) {
  FixedArrayScalar t;
  t.coords = {10.0, 20.0, 30.0};
  t.flags = {true, true};
  xmstream out;
  t.serialize(out);
  FixedArrayScalar t2;
  ASSERT_TRUE(t2.parse(reinterpret_cast<const char*>(out.data()), out.sizeOf()));
  EXPECT_DOUBLE_EQ(t2.coords[0], 10.0);
  EXPECT_DOUBLE_EQ(t2.coords[1], 20.0);
  EXPECT_DOUBLE_EQ(t2.coords[2], 30.0);
  EXPECT_EQ(t2.flags[0], true);
  EXPECT_EQ(t2.flags[1], true);
}

TEST(IdlTool, VerboseParseFixedArrayScalar) {
  const char *json = R"({"coords":[1.0,2.0,3.0],"flags":[true,false]})";
  FixedArrayScalar t;
  ParseError err;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), err));
  EXPECT_DOUBLE_EQ(t.coords[0], 1.0);
  EXPECT_DOUBLE_EQ(t.coords[2], 3.0);
}

TEST(IdlTool, ParseFixedArrayRejectsMissingComma) {
  const char *json = R"({"coords":[1.0 2.0,3.0],"flags":[true,false]})";
  FixedArrayScalar t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(IdlTool, VerboseParseFixedArrayRejectsMissingComma) {
  const char *json = R"({"coords":[1.0 2.0,3.0],"flags":[true,false]})";
  FixedArrayScalar t;
  ParseError err;
  EXPECT_FALSE(t.parseVerbose(json, strlen(json), err));
}

TEST(IdlTool, ParseFixedArrayStruct) {
  const char *json = R"({"pair":[{"value":"a","count":1},{"value":"b","count":2}]})";
  FixedArrayStruct t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.pair[0].value, "a");
  EXPECT_EQ(t.pair[0].count, 1);
  EXPECT_EQ(t.pair[1].value, "b");
  EXPECT_EQ(t.pair[1].count, 2);
}

TEST(IdlTool, ParseOptionalFixedArrayPresent) {
  const char *json = R"({"label":"test","data":[10,20,30]})";
  OptionalFixedArray t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.label, "test");
  ASSERT_TRUE(t.data.has_value());
  EXPECT_EQ((*t.data)[0], 10);
  EXPECT_EQ((*t.data)[1], 20);
  EXPECT_EQ((*t.data)[2], 30);
}

TEST(IdlTool, ParseOptionalFixedArrayAbsent) {
  const char *json = R"({"label":"test"})";
  OptionalFixedArray t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.label, "test");
  EXPECT_FALSE(t.data.has_value());
}

TEST(IdlTool, ParseOptionalFixedArrayNullRejected) {
  const char *json = R"({"label":"test","data":null})";
  OptionalFixedArray t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

// ============================================================================
// Multi-dimensional array tests
// ============================================================================

TEST(IdlTool, ParseMatrix2D) {
  const char *json = R"({"grid":[[1,2,3],[4,5,6]]})";
  Matrix2D t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.grid.size(), 2u);
  ASSERT_EQ(t.grid[0].size(), 3u);
  EXPECT_EQ(t.grid[0][0], 1);
  EXPECT_EQ(t.grid[0][2], 3);
  EXPECT_EQ(t.grid[1][0], 4);
  EXPECT_EQ(t.grid[1][2], 6);
}

TEST(IdlTool, SerializeMatrix2D) {
  Matrix2D t;
  t.grid = {{1, 2}, {3, 4, 5}};
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_EQ(doc["grid"].Size(), 2u);
  ASSERT_EQ(doc["grid"][0].Size(), 2u);
  EXPECT_EQ(doc["grid"][0][0].GetInt(), 1);
  EXPECT_EQ(doc["grid"][1][2].GetInt(), 5);
}

TEST(IdlTool, RoundtripMatrix2D) {
  Matrix2D t;
  t.grid = {{10, 20}, {30}};
  xmstream out;
  t.serialize(out);
  Matrix2D t2;
  ASSERT_TRUE(t2.parse(reinterpret_cast<const char*>(out.data()), out.sizeOf()));
  ASSERT_EQ(t2.grid.size(), 2u);
  EXPECT_EQ(t2.grid[0][0], 10);
  EXPECT_EQ(t2.grid[0][1], 20);
  EXPECT_EQ(t2.grid[1][0], 30);
}

TEST(IdlTool, VerboseParseMatrix2D) {
  const char *json = R"({"grid":[[1,2],[3,4]]})";
  Matrix2D t;
  ParseError err;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), err));
  ASSERT_EQ(t.grid.size(), 2u);
  EXPECT_EQ(t.grid[0][0], 1);
  EXPECT_EQ(t.grid[1][1], 4);
}

TEST(IdlTool, ParseFixedMatrixRejectsMissingInnerComma) {
  const char *json = R"({"mat":[[1.0,2.0 3.0],[4.0,5.0,6.0]]})";
  FixedMatrix t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(IdlTool, VerboseParseFixedMatrixRejectsMissingInnerComma) {
  const char *json = R"({"mat":[[1.0,2.0 3.0],[4.0,5.0,6.0]]})";
  FixedMatrix t;
  ParseError err;
  EXPECT_FALSE(t.parseVerbose(json, strlen(json), err));
}

TEST(IdlTool, ParseFixedMatrix) {
  const char *json = R"({"mat":[[1.0,2.0,3.0],[4.0,5.0,6.0]]})";
  FixedMatrix t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_DOUBLE_EQ(t.mat[0][0], 1.0);
  EXPECT_DOUBLE_EQ(t.mat[0][2], 3.0);
  EXPECT_DOUBLE_EQ(t.mat[1][0], 4.0);
  EXPECT_DOUBLE_EQ(t.mat[1][2], 6.0);
}

TEST(IdlTool, SerializeFixedMatrix) {
  FixedMatrix t;
  t.mat = {{{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}}};
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_EQ(doc["mat"].Size(), 2u);
  ASSERT_EQ(doc["mat"][0].Size(), 3u);
  EXPECT_DOUBLE_EQ(doc["mat"][0][0].GetDouble(), 1.0);
  EXPECT_DOUBLE_EQ(doc["mat"][1][2].GetDouble(), 6.0);
}

TEST(IdlTool, ParseMixedDims) {
  const char *json = R"({"rows":[[10,20],[30,40],[50,60]]})";
  MixedDims t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.rows.size(), 3u);
  EXPECT_EQ(t.rows[0][0], 10);
  EXPECT_EQ(t.rows[0][1], 20);
  EXPECT_EQ(t.rows[2][0], 50);
  EXPECT_EQ(t.rows[2][1], 60);
}

TEST(IdlTool, RoundtripMixedDims) {
  MixedDims t;
  t.rows = {{{1, 2}}, {{3, 4}}};
  xmstream out;
  t.serialize(out);
  MixedDims t2;
  ASSERT_TRUE(t2.parse(reinterpret_cast<const char*>(out.data()), out.sizeOf()));
  ASSERT_EQ(t2.rows.size(), 2u);
  EXPECT_EQ(t2.rows[0][0], 1);
  EXPECT_EQ(t2.rows[1][1], 4);
}

TEST(IdlTool, SerializeOptionalMatrix2DPresent) {
  OptionalMatrix2D t;
  t.label = "m";
  t.grid = std::vector<std::vector<int32_t>>{{1, 2}, {3, 4, 5}};
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["label"].GetString(), "m");
  ASSERT_TRUE(doc["grid"].IsArray());
  ASSERT_EQ(doc["grid"].Size(), 2u);
  EXPECT_EQ(doc["grid"][0][1].GetInt(), 2);
  EXPECT_EQ(doc["grid"][1][2].GetInt(), 5);
}

TEST(IdlTool, SerializeOptionalMatrix2DAbsent) {
  OptionalMatrix2D t;
  t.label = "m";
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_FALSE(doc.HasMember("grid"));
}

TEST(IdlTool, SerializeNullableMatrix2DNull) {
  NullableMatrix2D t;
  t.label = "m";
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.HasMember("grid"));
  EXPECT_TRUE(doc["grid"].IsNull());
}

TEST(IdlTool, RoundtripOptionalFixedMatrix) {
  OptionalFixedMatrix t;
  t.label = "fixed";
  t.mat = std::array<std::array<double, 2>, 2>{{{{1.0, 2.0}}, {{3.0, 4.0}}}};
  xmstream out;
  t.serialize(out);
  OptionalFixedMatrix t2;
  ASSERT_TRUE(t2.parse(reinterpret_cast<const char*>(out.data()), out.sizeOf()));
  ASSERT_TRUE(t2.mat.has_value());
  EXPECT_DOUBLE_EQ((*t2.mat)[0][0], 1.0);
  EXPECT_DOUBLE_EQ((*t2.mat)[1][1], 4.0);
}

// ============================================================================
// Nullable array elements (optional elements inside arrays)
// ============================================================================

TEST(IdlTool, ParseNullableStringArray) {
  const char json[] = R"({"strings":["hello",null,"world"],"numbers":[],"objects":[],"colors":[],"denied":[]})";
  NullableElements obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  ASSERT_EQ(obj.strings.size(), 3u);
  ASSERT_TRUE(obj.strings[0].has_value());
  EXPECT_EQ(*obj.strings[0], "hello");
  ASSERT_FALSE(obj.strings[1].has_value());
  ASSERT_TRUE(obj.strings[2].has_value());
  EXPECT_EQ(*obj.strings[2], "world");
}

TEST(IdlTool, ParseNullableIntArray) {
  const char json[] = R"({"strings":[],"numbers":[10,null,30],"objects":[],"colors":[],"denied":[]})";
  NullableElements obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  ASSERT_EQ(obj.numbers.size(), 3u);
  ASSERT_TRUE(obj.numbers[0].has_value());
  EXPECT_EQ(*obj.numbers[0], 10);
  ASSERT_FALSE(obj.numbers[1].has_value());
  ASSERT_TRUE(obj.numbers[2].has_value());
  EXPECT_EQ(*obj.numbers[2], 30);
}

TEST(IdlTool, ParseNullableStructArray) {
  const char json[] = R"({"strings":[],"numbers":[],"objects":[{"value":"a","count":1},null],"colors":[],"denied":[]})";
  NullableElements obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  ASSERT_EQ(obj.objects.size(), 2u);
  ASSERT_TRUE(obj.objects[0].has_value());
  EXPECT_EQ(obj.objects[0]->value, "a");
  EXPECT_EQ(obj.objects[0]->count, 1);
  ASSERT_FALSE(obj.objects[1].has_value());
}

TEST(IdlTool, ParseNullableEnumArray) {
  const char json[] = R"({"strings":[],"numbers":[],"objects":[],"colors":["red",null,"blue"],"denied":[]})";
  NullableElements obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  ASSERT_EQ(obj.colors.size(), 3u);
  ASSERT_TRUE(obj.colors[0].has_value());
  EXPECT_EQ(*obj.colors[0], EColor::red);
  ASSERT_FALSE(obj.colors[1].has_value());
  ASSERT_TRUE(obj.colors[2].has_value());
  EXPECT_EQ(*obj.colors[2], EColor::blue);
}

TEST(IdlTool, ParseNullableDenyRejectsNull) {
  const char json[] = R"({"strings":[],"numbers":[],"objects":[],"colors":[],"denied":["ok",null]})";
  NullableElements obj;
  EXPECT_FALSE(obj.parse(json, strlen(json)));
}

TEST(IdlTool, ParseNullableDenyAcceptsValues) {
  const char json[] = R"({"strings":[],"numbers":[],"objects":[],"colors":[],"denied":["a","b"]})";
  NullableElements obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  ASSERT_EQ(obj.denied.size(), 2u);
  EXPECT_TRUE(obj.denied[0].has_value());
  EXPECT_TRUE(obj.denied[1].has_value());
}

TEST(IdlTool, SerializeNullableElements) {
  NullableElements obj;
  obj.strings = {std::optional<std::string>("a"), std::nullopt, std::optional<std::string>("b")};
  obj.numbers = {std::optional<int64_t>(1), std::nullopt};
  xmstream out;
  obj.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_TRUE(doc["strings"].IsArray());
  ASSERT_EQ(doc["strings"].Size(), 3u);
  EXPECT_STREQ(doc["strings"][0].GetString(), "a");
  EXPECT_TRUE(doc["strings"][1].IsNull());
  EXPECT_STREQ(doc["strings"][2].GetString(), "b");
  ASSERT_TRUE(doc["numbers"].IsArray());
  ASSERT_EQ(doc["numbers"].Size(), 2u);
  EXPECT_EQ(doc["numbers"][0].GetInt64(), 1);
  EXPECT_TRUE(doc["numbers"][1].IsNull());
}

TEST(IdlTool, RoundtripNullableStringArray) {
  const char json[] = R"({"strings":["x",null,"y"],"numbers":[],"objects":[],"colors":[],"denied":[]})";
  NullableElements obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  xmstream out;
  obj.serialize(out);
  NullableElements obj2;
  ASSERT_TRUE(obj2.parse((const char*)out.data(), out.sizeOf()));
  ASSERT_EQ(obj2.strings.size(), 3u);
  EXPECT_EQ(*obj2.strings[0], "x");
  EXPECT_FALSE(obj2.strings[1].has_value());
  EXPECT_EQ(*obj2.strings[2], "y");
}

TEST(IdlTool, VerboseParseNullableElements) {
  const char json[] = R"({"strings":["a",null],"numbers":[null,5],"objects":[],"colors":[],"denied":[]})";
  NullableElements obj;
  ParseError error;
  ASSERT_TRUE(obj.parseVerbose(json, strlen(json), error));
  ASSERT_EQ(obj.strings.size(), 2u);
  EXPECT_TRUE(obj.strings[0].has_value());
  EXPECT_FALSE(obj.strings[1].has_value());
  ASSERT_EQ(obj.numbers.size(), 2u);
  EXPECT_FALSE(obj.numbers[0].has_value());
  EXPECT_TRUE(obj.numbers[1].has_value());
}

TEST(IdlTool, VerboseParseNullableDenyRejectsNull) {
  const char json[] = R"({"strings":[],"numbers":[],"objects":[],"colors":[],"denied":[null]})";
  NullableElements obj;
  ParseError error;
  EXPECT_FALSE(obj.parseVerbose(json, strlen(json), error));
  EXPECT_FALSE(error.message.empty());
}

// Nullable element combos: optional array, fixed array, matrix
TEST(IdlTool, ParseNullableFixedArray) {
  const char json[] = R"({"fixedArray":[1,null,3],"matrix":[]})";
  NullableElementCombos obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  ASSERT_TRUE(obj.fixedArray[0].has_value());
  EXPECT_EQ(*obj.fixedArray[0], 1);
  ASSERT_FALSE(obj.fixedArray[1].has_value());
  ASSERT_TRUE(obj.fixedArray[2].has_value());
  EXPECT_EQ(*obj.fixedArray[2], 3);
}

TEST(IdlTool, ParseNullableMatrix) {
  const char json[] = R"({"fixedArray":[null,null,null],"matrix":[[1,null],[null,2]]})";
  NullableElementCombos obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  ASSERT_EQ(obj.matrix.size(), 2u);
  ASSERT_EQ(obj.matrix[0].size(), 2u);
  EXPECT_TRUE(obj.matrix[0][0].has_value());
  EXPECT_EQ(*obj.matrix[0][0], 1);
  EXPECT_FALSE(obj.matrix[0][1].has_value());
  EXPECT_FALSE(obj.matrix[1][0].has_value());
  EXPECT_TRUE(obj.matrix[1][1].has_value());
}

TEST(IdlTool, ParseOptionalNullableArray) {
  // optionalArray absent
  const char json1[] = R"({"fixedArray":[null,null,null],"matrix":[]})";
  NullableElementCombos obj1;
  ASSERT_TRUE(obj1.parse(json1, strlen(json1)));
  EXPECT_FALSE(obj1.optionalArray.has_value());

  // optionalArray present with nullable elements
  const char json2[] = R"({"optionalArray":["a",null],"fixedArray":[null,null,null],"matrix":[]})";
  NullableElementCombos obj2;
  ASSERT_TRUE(obj2.parse(json2, strlen(json2)));
  ASSERT_TRUE(obj2.optionalArray.has_value());
  ASSERT_EQ(obj2.optionalArray->size(), 2u);
  EXPECT_TRUE((*obj2.optionalArray)[0].has_value());
  EXPECT_FALSE((*obj2.optionalArray)[1].has_value());
}

TEST(IdlTool, RoundtripNullableMatrix) {
  NullableElementCombos obj;
  obj.fixedArray = {std::optional<int32_t>(10), std::nullopt, std::optional<int32_t>(30)};
  obj.matrix = {{std::optional<int32_t>(1), std::nullopt}, {std::nullopt}};
  xmstream out;
  obj.serialize(out);
  NullableElementCombos obj2;
  ASSERT_TRUE(obj2.parse((const char*)out.data(), out.sizeOf()));
  EXPECT_EQ(*obj2.fixedArray[0], 10);
  EXPECT_FALSE(obj2.fixedArray[1].has_value());
  EXPECT_EQ(*obj2.fixedArray[2], 30);
  ASSERT_EQ(obj2.matrix.size(), 2u);
  EXPECT_EQ(*obj2.matrix[0][0], 1);
  EXPECT_FALSE(obj2.matrix[0][1].has_value());
  EXPECT_FALSE(obj2.matrix[1][0].has_value());
}

// ============================================================================
// Variant array elements
// ============================================================================

TEST(IdlTool, ParseVariantElementArray) {
  const char json[] = R"({"scalars":["hello",42,"world"],"mixed":[]})";
  VariantElements obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  ASSERT_EQ(obj.scalars.size(), 3u);
  EXPECT_EQ(std::get<std::string>(obj.scalars[0]), "hello");
  EXPECT_EQ(std::get<int64_t>(obj.scalars[1]), 42);
  EXPECT_EQ(std::get<std::string>(obj.scalars[2]), "world");
}

TEST(IdlTool, ParseVariantStructElementArray) {
  const char json[] = R"({"scalars":[],"mixed":[{"value":"test","count":5},"fallback"]})";
  VariantElements obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  ASSERT_EQ(obj.mixed.size(), 2u);
  EXPECT_EQ(std::get<Inner>(obj.mixed[0]).value, "test");
  EXPECT_EQ(std::get<Inner>(obj.mixed[0]).count, 5);
  EXPECT_EQ(std::get<std::string>(obj.mixed[1]), "fallback");
}

TEST(IdlTool, SerializeVariantElementArray) {
  VariantElements obj;
  obj.scalars.emplace_back(std::string("abc"));
  obj.scalars.emplace_back(int64_t(99));
  xmstream out;
  obj.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_TRUE(doc["scalars"].IsArray());
  ASSERT_EQ(doc["scalars"].Size(), 2u);
  EXPECT_STREQ(doc["scalars"][0].GetString(), "abc");
  EXPECT_EQ(doc["scalars"][1].GetInt64(), 99);
}

TEST(IdlTool, RoundtripVariantElementArray) {
  const char json[] = R"({"scalars":["a",1,"b",2],"mixed":[{"value":"v","count":0},"str"]})";
  VariantElements obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  xmstream out;
  obj.serialize(out);
  VariantElements obj2;
  ASSERT_TRUE(obj2.parse((const char*)out.data(), out.sizeOf()));
  ASSERT_EQ(obj2.scalars.size(), 4u);
  EXPECT_EQ(std::get<std::string>(obj2.scalars[0]), "a");
  EXPECT_EQ(std::get<int64_t>(obj2.scalars[1]), 1);
  EXPECT_EQ(std::get<std::string>(obj2.scalars[2]), "b");
  EXPECT_EQ(std::get<int64_t>(obj2.scalars[3]), 2);
  ASSERT_EQ(obj2.mixed.size(), 2u);
  EXPECT_EQ(std::get<Inner>(obj2.mixed[0]).value, "v");
  EXPECT_EQ(std::get<std::string>(obj2.mixed[1]), "str");
}

TEST(IdlTool, VerboseParseVariantElementArray) {
  const char json[] = R"({"scalars":["x",10],"mixed":[]})";
  VariantElements obj;
  ParseError error;
  ASSERT_TRUE(obj.parseVerbose(json, strlen(json), error));
  ASSERT_EQ(obj.scalars.size(), 2u);
  EXPECT_EQ(std::get<std::string>(obj.scalars[0]), "x");
  EXPECT_EQ(std::get<int64_t>(obj.scalars[1]), 10);
}

// ============================================================================
// Map array elements
// ============================================================================

TEST(IdlTool, ParseMapElementArray) {
  const char json[] = R"({"scalars":[{"a":1,"b":2},{"c":3}],"objects":[]})";
  MapElements obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  ASSERT_EQ(obj.scalars.size(), 2u);
  EXPECT_EQ(obj.scalars[0].at("a"), 1);
  EXPECT_EQ(obj.scalars[0].at("b"), 2);
  EXPECT_EQ(obj.scalars[1].at("c"), 3);
}

TEST(IdlTool, ParseMapStructElementArray) {
  const char json[] = R"({"scalars":[],"objects":[{"x":{"value":"v","count":1}}]})";
  MapElements obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  ASSERT_EQ(obj.objects.size(), 1u);
  EXPECT_EQ(obj.objects[0].at("x").value, "v");
  EXPECT_EQ(obj.objects[0].at("x").count, 1);
}

TEST(IdlTool, SerializeMapElementArray) {
  MapElements obj;
  obj.scalars.push_back({{"k", 42}});
  xmstream out;
  obj.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_TRUE(doc["scalars"].IsArray());
  ASSERT_EQ(doc["scalars"].Size(), 1u);
  ASSERT_TRUE(doc["scalars"][0].IsObject());
  EXPECT_EQ(doc["scalars"][0]["k"].GetInt64(), 42);
}

TEST(IdlTool, RoundtripMapElementArray) {
  MapElements obj;
  obj.scalars.push_back({{"x", 10}, {"y", 20}});
  obj.scalars.push_back({{"z", 30}});
  xmstream out;
  obj.serialize(out);
  MapElements obj2;
  ASSERT_TRUE(obj2.parse((const char*)out.data(), out.sizeOf()));
  ASSERT_EQ(obj2.scalars.size(), 2u);
  EXPECT_EQ(obj2.scalars[0].at("x"), 10);
  EXPECT_EQ(obj2.scalars[0].at("y"), 20);
  EXPECT_EQ(obj2.scalars[1].at("z"), 30);
}

TEST(IdlTool, VerboseParseMapElementArray) {
  const char json[] = R"({"scalars":[{"a":1}],"objects":[]})";
  MapElements obj;
  ParseError error;
  ASSERT_TRUE(obj.parseVerbose(json, strlen(json), error));
  ASSERT_EQ(obj.scalars.size(), 1u);
  EXPECT_EQ(obj.scalars[0].at("a"), 1);
}

TEST(IdlTool, ParseEmptyMapElementArray) {
  const char json[] = R"({"scalars":[{}],"objects":[]})";
  MapElements obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  ASSERT_EQ(obj.scalars.size(), 1u);
  EXPECT_TRUE(obj.scalars[0].empty());
}
