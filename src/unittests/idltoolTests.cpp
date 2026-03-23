#include "gtest/gtest.h"
#include "test.idl.h"
#include "p2putils/xmstream.h"
#include "rapidjson/document.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static rapidjson::Document parseRapid(const xmstream &stream) {
  rapidjson::Document doc;
  doc.Parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf());
  return doc;
}

namespace {

struct CommandResult {
  int ExitCode = -1;
  std::string Output;
};

class TempDir {
public:
  explicit TempDir(std::filesystem::path path) : Path_(std::move(path)) {}

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(Path_, ec);
  }

  const std::filesystem::path &path() const { return Path_; }

private:
  std::filesystem::path Path_;
};

static std::filesystem::path makeTempDir() {
  static uint64_t counter = 0;
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  auto base = std::filesystem::temp_directory_path();

  for (uint64_t attempt = 0; attempt < 1024; ++attempt) {
    auto dir = base / ("poolcore-idltool-tests-" + std::to_string(now) + "-" + std::to_string(counter++));
    std::error_code ec;
    if (std::filesystem::create_directory(dir, ec))
      return dir;
  }

  throw std::runtime_error("failed to create temporary directory");
}

static void writeTextFile(const std::filesystem::path &path, const std::string &content) {
  std::ofstream out(path, std::ios::binary);
  if (!out)
    throw std::runtime_error("failed to open " + path.string());
  out << content;
  if (!out)
    throw std::runtime_error("failed to write " + path.string());
}

static std::string readTextFile(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("failed to open " + path.string());
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

static std::string shellQuote(const std::string &value) {
  std::string quoted = "'";
  for (char ch : value) {
    if (ch == '\'')
      quoted += "'\\''";
    else
      quoted += ch;
  }
  quoted += "'";
  return quoted;
}

static CommandResult runCommandCapture(const std::string &command, const std::filesystem::path &logPath) {
  CommandResult result;
  std::string wrapped = command + " > " + shellQuote(logPath.string()) + " 2>&1";
  result.ExitCode = std::system(wrapped.c_str());
  result.Output = readTextFile(logPath);
  return result;
}

static testing::AssertionResult generatedIdlFails(const std::string &stem, const std::string &idlSource,
    const std::string &needle = "") {
  try {
    TempDir tempDir(makeTempDir());
    auto idlPath = tempDir.path() / (stem + ".idl");
    auto headerPath = tempDir.path() / (stem + ".idl.h");
    auto genLogPath = tempDir.path() / "idltool.log";

    writeTextFile(tempDir.path() / "support.h", "#pragma once\n");
    writeTextFile(idlPath, idlSource);

    std::string generateCmd =
      shellQuote(IDLTOOL_BINARY_PATH) + " " +
      shellQuote(idlPath.string()) + " -o " +
      shellQuote(headerPath.string());
    auto genResult = runCommandCapture(generateCmd, genLogPath);
    if (genResult.ExitCode == 0) {
      return testing::AssertionFailure()
        << "idltool unexpectedly succeeded for " << idlPath.filename().string() << "\n"
        << genResult.Output;
    }

    if (!needle.empty() && genResult.Output.find(needle) == std::string::npos) {
      return testing::AssertionFailure()
        << "idltool failed for " << idlPath.filename().string()
        << ", but output did not contain expected fragment '" << needle << "'\n"
        << genResult.Output;
    }

    return testing::AssertionSuccess();
  } catch (const std::exception &e) {
    return testing::AssertionFailure() << e.what();
  }
}

static testing::AssertionResult generatedIdlSourceContains(const std::string &stem, const std::string &idlSource,
    std::initializer_list<std::string> needles) {
  try {
    TempDir tempDir(makeTempDir());
    auto idlPath = tempDir.path() / (stem + ".idl");
    auto headerPath = tempDir.path() / (stem + ".idl.h");
    auto sourcePath = tempDir.path() / (stem + ".idl.cpp");
    auto genLogPath = tempDir.path() / "idltool.log";

    writeTextFile(tempDir.path() / "support.h", "#pragma once\n");
    writeTextFile(idlPath, idlSource);

    std::string generateCmd =
      shellQuote(IDLTOOL_BINARY_PATH) + " " +
      shellQuote(idlPath.string()) + " -o " +
      shellQuote(headerPath.string());
    auto genResult = runCommandCapture(generateCmd, genLogPath);
    if (genResult.ExitCode != 0) {
      return testing::AssertionFailure()
        << "idltool failed for " << idlPath.filename().string() << "\n"
        << genResult.Output;
    }

    std::string source = readTextFile(sourcePath);
    for (const std::string &needle : needles) {
      if (source.find(needle) == std::string::npos) {
        return testing::AssertionFailure()
          << "generated source for " << idlPath.filename().string()
          << " did not contain expected fragment '" << needle << "'\n"
          << source;
      }
    }

    return testing::AssertionSuccess();
  } catch (const std::exception &e) {
    return testing::AssertionFailure() << e.what();
  }
}

static testing::AssertionResult generatedIdlHeaderContains(const std::string &stem, const std::string &idlSource,
    std::initializer_list<std::string> needles) {
  try {
    TempDir tempDir(makeTempDir());
    auto idlPath = tempDir.path() / (stem + ".idl");
    auto headerPath = tempDir.path() / (stem + ".idl.h");
    auto genLogPath = tempDir.path() / "idltool.log";

    writeTextFile(tempDir.path() / "support.h", "#pragma once\n");
    writeTextFile(idlPath, idlSource);

    std::string generateCmd =
      shellQuote(IDLTOOL_BINARY_PATH) + " " +
      shellQuote(idlPath.string()) + " -o " +
      shellQuote(headerPath.string());
    auto genResult = runCommandCapture(generateCmd, genLogPath);
    if (genResult.ExitCode != 0) {
      return testing::AssertionFailure()
        << "idltool failed for " << idlPath.filename().string() << "\n"
        << genResult.Output;
    }

    std::string header = readTextFile(headerPath);
    for (const std::string &needle : needles) {
      if (header.find(needle) == std::string::npos) {
        return testing::AssertionFailure()
          << "generated header for " << idlPath.filename().string()
          << " did not contain expected fragment '" << needle << "'\n"
          << header;
      }
    }

    return testing::AssertionSuccess();
  } catch (const std::exception &e) {
    return testing::AssertionFailure() << e.what();
  }
}

static testing::AssertionResult generatedIdlSourceOmits(const std::string &stem, const std::string &idlSource,
    std::initializer_list<std::string> needles) {
  try {
    TempDir tempDir(makeTempDir());
    auto idlPath = tempDir.path() / (stem + ".idl");
    auto headerPath = tempDir.path() / (stem + ".idl.h");
    auto sourcePath = tempDir.path() / (stem + ".idl.cpp");
    auto genLogPath = tempDir.path() / "idltool.log";

    writeTextFile(tempDir.path() / "support.h", "#pragma once\n");
    writeTextFile(idlPath, idlSource);

    std::string generateCmd =
      shellQuote(IDLTOOL_BINARY_PATH) + " " +
      shellQuote(idlPath.string()) + " -o " +
      shellQuote(headerPath.string());
    auto genResult = runCommandCapture(generateCmd, genLogPath);
    if (genResult.ExitCode != 0) {
      return testing::AssertionFailure()
        << "idltool failed for " << idlPath.filename().string() << "\n"
        << genResult.Output;
    }

    std::string source = readTextFile(sourcePath);
    for (const std::string &needle : needles) {
      if (source.find(needle) != std::string::npos) {
        return testing::AssertionFailure()
          << "generated source for " << idlPath.filename().string()
          << " unexpectedly contained fragment '" << needle << "'\n"
          << source;
      }
    }

    return testing::AssertionSuccess();
  } catch (const std::exception &e) {
    return testing::AssertionFailure() << e.what();
  }
}

} // namespace

template<typename T>
concept ParseSerializeWithNoCtx = requires(T value,
                                           const typename T::Capture &capture,
                                           xmstream &out) {
  value.serialize(out);
  T::resolve(value, capture);
};

template<typename T>
concept ParseSerializeWithOneCtx = requires(T value,
                                            const typename T::Capture &capture,
                                            xmstream &out) {
  value.serialize(out, uint32_t{});
  T::resolve(value, capture, uint32_t{});
};

template<typename T>
concept ParseSerializeWithTwoCtx = requires(T value,
                                            const typename T::Capture &capture,
                                            xmstream &out) {
  value.serialize(out, uint32_t{}, uint32_t{});
  T::resolve(value, capture, uint32_t{}, uint32_t{});
};

template<typename TParent, typename TField>
concept FlatSerializeWithOneCtx = requires(const TField &field, xmstream &out) {
  TParent::serialize(out, field, uint32_t{});
};

template<typename TParent, typename TField>
concept FlatSerializeWithTwoCtx = requires(const TField &field, xmstream &out) {
  TParent::serialize(out, field, uint32_t{}, uint32_t{});
};

static_assert(ParseSerializeWithOneCtx<CtxFixedArrayParent>);
static_assert(!ParseSerializeWithNoCtx<CtxFixedArrayParent>);

static_assert(ParseSerializeWithOneCtx<CtxFixedMatrixParent>);
static_assert(!ParseSerializeWithNoCtx<CtxFixedMatrixParent>);

static_assert(ParseSerializeWithOneCtx<CtxDirectMappedArrayParent>);
static_assert(!ParseSerializeWithNoCtx<CtxDirectMappedArrayParent>);

static_assert(ParseSerializeWithOneCtx<CtxNumericWireParent>);
static_assert(!ParseSerializeWithNoCtx<CtxNumericWireParent>);

static_assert(ParseSerializeWithTwoCtx<CtxDuplicateMappedGroupsParent>);
static_assert(!ParseSerializeWithOneCtx<CtxDuplicateMappedGroupsParent>);
static_assert(FlatSerializeWithTwoCtx<CtxDuplicateMappedGroupsParent, CtxDuplicateMappedGroupsChild>);
static_assert(!FlatSerializeWithOneCtx<CtxDuplicateMappedGroupsParent, CtxDuplicateMappedGroupsChild>);

static_assert(ParseSerializeWithTwoCtx<CtxMultipleDependenciesParent>);
static_assert(!ParseSerializeWithOneCtx<CtxMultipleDependenciesParent>);

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

TEST(IdlTool, SerializeMergesAdjacentLiteralWrites) {
  const std::string idl = R"(
struct MergeWrites {
  first: string;
  second: uint32;
  items: [string];
}
)";

  EXPECT_TRUE(generatedIdlSourceContains("merge-writes", idl, {
    "out.write(\"{\\\"first\\\":\");",
    "out.write(\",\\\"second\\\":\");",
    "out.write(\",\\\"items\\\":[\");",
    "out.write(\"]}\");"
  }));
}

TEST(IdlTool, EmitOnlyUsedJsonWriteHelpers) {
  const std::string idl = R"(
struct BoolOnly {
  .generate(serialize);
  enabled: bool;
}
)";

  EXPECT_TRUE(generatedIdlSourceContains("bool-only-serialize", idl, {
    "#include \"idltool/jsonWriteBool.h\""
  }));
  EXPECT_TRUE(generatedIdlSourceOmits("bool-only-serialize", idl, {
    "jsonWriteString.h",
    "jsonWriteInt.h",
    "jsonWriteUInt.h",
    "jsonWriteDouble.h"
  }));
}

TEST(IdlTool, OmitJsonWriteHelpersWithoutSerializeGeneration) {
  const std::string idl = R"(
struct ParseOnly {
  .generate(parse);
  count: uint64;
}
)";

  EXPECT_TRUE(generatedIdlSourceOmits("parse-only-no-serialize", idl, {
    "jsonWriteString.h",
    "jsonWriteInt.h",
    "jsonWriteUInt.h",
    "jsonWriteDouble.h",
    "jsonWriteBool.h"
  }));
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
  EXPECT_DOUBLE_EQ(d.fieldNegDouble, -3.14);
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

  const char *json2 = R"({"name":"y"})";
  OptionalChildren oc2;
  ASSERT_TRUE(oc2.parse(json2, strlen(json2)));
  EXPECT_FALSE(oc2.first.has_value());
  EXPECT_FALSE(oc2.second.has_value());
}

TEST(IdlTool, ParseOptionalObjectNullRejected) {
  const char *json = R"({"name":"y","first":null})";
  OptionalChildren oc;
  EXPECT_FALSE(oc.parse(json, strlen(json)));
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

TEST(IdlTool, ParseOptionalPolicyScalars) {
  const char *json = R"({"denyOmit":"a","denyNull":"b","allowOmit":null,"allowNull":"c","requiredNull":null})";
  OptionalPolicyScalars s;
  ASSERT_TRUE(s.parse(json, strlen(json)));
  ASSERT_TRUE(s.denyOmit.has_value());
  EXPECT_EQ(*s.denyOmit, "a");
  ASSERT_TRUE(s.denyNull.has_value());
  EXPECT_EQ(*s.denyNull, "b");
  EXPECT_FALSE(s.allowOmit.has_value());
  ASSERT_TRUE(s.allowNull.has_value());
  EXPECT_EQ(*s.allowNull, "c");
  EXPECT_FALSE(s.requiredNull.has_value());
}

TEST(IdlTool, ParseOptionalPolicyScalarsAbsentAndRequired) {
  const char *json = R"({"denyOmit":"a","denyNull":"b","allowNull":null})";
  OptionalPolicyScalars s;
  EXPECT_FALSE(s.parse(json, strlen(json)));
}

TEST(IdlTool, ParseOptionalPolicyScalarsNullDenied) {
  const char *json = R"({"denyOmit":null,"denyNull":"b","requiredNull":"x"})";
  OptionalPolicyScalars s;
  EXPECT_FALSE(s.parse(json, strlen(json)));
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

TEST(IdlTool, ParseSkipUnknown) {
  // Various unknown value types: string, nested object, array, null, bool, number
  {
    const char *json = R"({"a":null,"value":"x","b":true,"nested":{"deep":{"k":"v"},"arr":[1,2]},"c":42.5,"count":1,"extra":[1,"two",null],"d":false})";
    SkipUnknown obj;
    ASSERT_TRUE(obj.parse(json, strlen(json)));
    EXPECT_EQ(obj.value, "x");
    EXPECT_EQ(obj.count, 1);
  }

  // Verbose parse also skips
  {
    const char *json = R"({"value":"x","count":1,"extra":"ignored"})";
    SkipUnknown obj;
    ParseError error;
    ASSERT_TRUE(obj.parseVerbose(json, strlen(json), error));
    EXPECT_EQ(obj.value, "x");
    EXPECT_EQ(obj.count, 1);
  }

  // Required fields still enforced
  {
    const char *json = R"({"value":"x","extra":"ignored"})";
    SkipUnknown obj;
    EXPECT_FALSE(obj.parse(json, strlen(json)));
  }
}

TEST(IdlTool, KeywordsAsFieldNames) {
  const char *json = R"({"type":"t","context":1,"mapped":"m","variant":true,"generate":99,"extensions":"e","ctxgroup":3.14,"flags":42,"class":"cls","return":7,"class_":"trailing","map":55})";
  KeywordsAsFields obj;
  ASSERT_TRUE(obj.parse(json, strlen(json)));
  EXPECT_EQ(obj.type, "t");
  EXPECT_EQ(obj.context, 1);
  EXPECT_EQ(obj.mapped, "m");
  EXPECT_EQ(obj.variant, true);
  EXPECT_EQ(obj.generate, 99);
  EXPECT_EQ(obj.extensions, "e");
  EXPECT_DOUBLE_EQ(obj.ctxgroup, 3.14);
  EXPECT_EQ(obj.flags, 42u);
  // C++ keywords mangled: "class" → class_, "return" → return_
  EXPECT_EQ(obj.class_, "cls");
  EXPECT_EQ(obj.return_, 7);
  // "class_" ends with _ (demangle collision) → class__
  EXPECT_EQ(obj.class__, "trailing");
  EXPECT_EQ(obj.map, 55);

  // Roundtrip: serialize must produce original JSON keys
  xmstream stream;
  obj.serialize(stream);
  KeywordsAsFields obj2;
  ASSERT_TRUE(obj2.parse(reinterpret_cast<const char*>(stream.data()), stream.sizeOf()));
  EXPECT_EQ(obj2.type, "t");
  EXPECT_EQ(obj2.generate, 99);
  EXPECT_EQ(obj2.flags, 42u);
  EXPECT_EQ(obj2.class_, "cls");
  EXPECT_EQ(obj2.return_, 7);
  EXPECT_EQ(obj2.class__, "trailing");
  EXPECT_EQ(obj2.map, 55);
}

TEST(IdlTool, ParseWithComments) {
  const char *json =
    "{\n"
    "  // inline comment\n"
    "  \"value\": \"x\", /* block comment */\n"
    "  \"count\": 1\n"
    "}";
  InnerWithComments inner;
  ASSERT_TRUE(inner.parse(json, strlen(json)));
  EXPECT_EQ(inner.value, "x");
  EXPECT_EQ(inner.count, 1);
}

TEST(IdlTool, ParseCommentsRejectedWithoutExtension) {
  const char *json =
    "{\n"
    "  // inline comment\n"
    "  \"value\": \"x\",\n"
    "  \"count\": 1\n"
    "}";
  Inner inner;
  EXPECT_FALSE(inner.parse(json, strlen(json)));
}

TEST(IdlTool, ParseCommentsInNestedStruct) {
  const char *json =
    "{\n"
    "  /* comment before label */\n"
    "  \"label\": \"test\",\n"
    "  \"child\": {\n"
    "    // comment inside nested struct\n"
    "    \"value\": \"nested\",\n"
    "    \"count\": 42\n"
    "  }\n"
    "}";
  OuterWithComments outer;
  ASSERT_TRUE(outer.parse(json, strlen(json)));
  EXPECT_EQ(outer.label, "test");
  EXPECT_EQ(outer.child.value, "nested");
  EXPECT_EQ(outer.child.count, 42);
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

TEST(IdlTool, ParseInlineMappedOptionalNullRejected) {
  const char *json = R"({"amount":"42","optionalAmount":null})";
  InlineMapped m;
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
  m.optional3 = true;

  xmstream stream;
  m.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["required1"].GetString(), "r1");
  EXPECT_EQ(doc["required2"].GetInt64(), 123);
  EXPECT_EQ(doc["required3"].GetBool(), true);
  EXPECT_STREQ(doc["optional1"].GetString(), "opt");
  EXPECT_EQ(doc["optional2"].GetInt64(), 456);
  EXPECT_EQ(doc["optional3"].GetBool(), true);
}

TEST(IdlTool, SerializeDefaultsAlwaysPresent) {
  ScalarDefaults d;

  xmstream stream;
  d.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.IsObject());
  EXPECT_EQ(doc.MemberCount(), 8u);

  EXPECT_STREQ(doc["fieldString"].GetString(), "hello");
  EXPECT_EQ(doc["fieldBool"].GetBool(), true);
  EXPECT_EQ(doc["fieldInt32"].GetInt(), -1);
  EXPECT_EQ(doc["fieldUint32"].GetUint(), 42u);
  EXPECT_EQ(doc["fieldInt64"].GetInt64(), 0);
  EXPECT_EQ(doc["fieldUint64"].GetUint64(), 100u);
  EXPECT_DOUBLE_EQ(doc["fieldDouble"].GetDouble(), 3.14);
  EXPECT_DOUBLE_EQ(doc["fieldNegDouble"].GetDouble(), -3.14);
}

TEST(IdlTool, SerializeDefaultsOverridden) {
  ScalarDefaults d;
  d.fieldDouble = 4.5;

  xmstream stream;
  d.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.IsObject());
  EXPECT_EQ(doc.MemberCount(), 8u);
  EXPECT_DOUBLE_EQ(doc["fieldDouble"].GetDouble(), 4.5);

  // other fields still present with their defaults
  EXPECT_STREQ(doc["fieldString"].GetString(), "hello");
  EXPECT_EQ(doc["fieldBool"].GetBool(), true);
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

TEST(IdlTool, SerializeOptionalPolicyScalars) {
  OptionalPolicyScalars s;

  xmstream stream1;
  s.serialize(stream1);
  auto doc1 = parseRapid(stream1);
  ASSERT_FALSE(doc1.HasParseError());
  EXPECT_FALSE(doc1.HasMember("denyOmit"));
  ASSERT_TRUE(doc1.HasMember("denyNull"));
  EXPECT_TRUE(doc1["denyNull"].IsNull());
  EXPECT_FALSE(doc1.HasMember("allowOmit"));
  ASSERT_TRUE(doc1.HasMember("allowNull"));
  EXPECT_TRUE(doc1["allowNull"].IsNull());
  ASSERT_TRUE(doc1.HasMember("requiredNull"));
  EXPECT_TRUE(doc1["requiredNull"].IsNull());

  s.denyOmit = "a";
  s.allowOmit = "b";
  s.requiredNull = "c";
  xmstream stream2;
  s.serialize(stream2);
  auto doc2 = parseRapid(stream2);
  ASSERT_FALSE(doc2.HasParseError());
  EXPECT_STREQ(doc2["denyOmit"].GetString(), "a");
  EXPECT_STREQ(doc2["allowOmit"].GetString(), "b");
  EXPECT_STREQ(doc2["requiredNull"].GetString(), "c");
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
  // CtxChild has a single context field, so it gets an implicit own group.
  // CtxVectorParent wraps that child in an array, so it takes vector<uint32_t>.
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

TEST(IdlTool, BugUint32WireSerialize) {
  WithUint32Mapped m;
  m.counter = 4000000000U;

  xmstream stream;
  m.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["counter"].IsUint());
  EXPECT_EQ(doc["counter"].GetUint(), 4000000000U);
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

// Bug: surrogate pairs (\uD83D\uDE00 = U+1F600 😀) are not combined;
// each half is independently encoded as a 3-byte BMP sequence (invalid UTF-8).
TEST(IdlTool, BugSurrogatePair) {
  // \uD83D\uDE00 is JSON encoding for U+1F600 (grinning face emoji)
  const char *json = R"({"value":"\uD83D\uDE00","count":1})";
  Inner inner;
  ASSERT_TRUE(inner.parse(json, strlen(json)));
  // U+1F600 in UTF-8: F0 9F 98 80
  EXPECT_EQ(inner.value, "\xF0\x9F\x98\x80");
}

TEST(IdlTool, BugSurrogatePairVerbose) {
  const char *json = R"({"value":"\uD83D\uDE00","count":1})";
  Inner inner;
  ParseError error;
  ASSERT_TRUE(inner.parseVerbose(json, strlen(json), error));
  EXPECT_EQ(inner.value, "\xF0\x9F\x98\x80");
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

// ============================================================================
// Coverage gap tests
// ============================================================================

// --- Nullable struct object ---

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
  // empty_out=null emits null (unlike default optional which omits)
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
  const char *json = R"({"name":"x","first":null})";
  OptionalChildren oc;
  ParseError error;
  EXPECT_FALSE(oc.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("null is not allowed"), std::string::npos);
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

TEST(IdlTool, VerboseParseOptionalPolicyScalarsRequiredMissing) {
  const char *json = R"({"denyOmit":"a"})";
  OptionalPolicyScalars s;
  ParseError error;
  EXPECT_FALSE(s.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("missing required field 'requiredNull'"), std::string::npos);
}

TEST(IdlTool, VerboseParseOptionalPolicyScalarsNullDenied) {
  const char *json = R"({"denyOmit":null,"requiredNull":"x"})";
  OptionalPolicyScalars s;
  ParseError error;
  EXPECT_FALSE(s.parseVerbose(json, strlen(json), error));
  EXPECT_NE(error.message.find("null is not allowed"), std::string::npos);
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

TEST(IdlTool, ContextSplitGroupsSerialize) {
  CtxSplitAmountsParent parent;
  parent.leftAmounts = {12345, 600};
  parent.rightAmounts = {67890, 7000};

  xmstream stream;
  parent.serialize(stream, 2, 3);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["leftAmounts"].IsArray());
  ASSERT_TRUE(doc["rightAmounts"].IsArray());
  EXPECT_STREQ(doc["leftAmounts"][0].GetString(), "123.45");
  EXPECT_STREQ(doc["leftAmounts"][1].GetString(), "6.00");
  EXPECT_STREQ(doc["rightAmounts"][0].GetString(), "67.890");
  EXPECT_STREQ(doc["rightAmounts"][1].GetString(), "7.000");
}

TEST(IdlTool, ContextSplitGroupsResolve) {
  const char *json = R"({"leftAmounts":["123.45","6.00"],"rightAmounts":["67.890","7.000"]})";

  CtxSplitAmountsParent parent;
  CtxSplitAmountsParent::Capture capture;
  ASSERT_TRUE(parent.parse(json, strlen(json), capture));

  ASSERT_TRUE(CtxSplitAmountsParent::resolve(parent, capture, 2, 3));
  ASSERT_EQ(parent.leftAmounts.size(), 2u);
  ASSERT_EQ(parent.rightAmounts.size(), 2u);
  EXPECT_EQ(parent.leftAmounts[0], 12345);
  EXPECT_EQ(parent.leftAmounts[1], 600);
  EXPECT_EQ(parent.rightAmounts[0], 67890);
  EXPECT_EQ(parent.rightAmounts[1], 7000);
}

TEST(IdlTool, ContextSharedGroupSerialize) {
  CtxSharedAmountsParent parent;
  parent.leftAmounts = {12345};
  parent.rightAmounts = {67890};

  xmstream stream;
  parent.serialize(stream, 3);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["leftAmounts"][0].GetString(), "12.345");
  EXPECT_STREQ(doc["rightAmounts"][0].GetString(), "67.890");
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
  // Default optional objects should NOT appear in output when absent
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

// ============================================================================
// Variant tests
// ============================================================================

TEST(IdlTool, ParseVariantString) {
  const char *json = R"({"value":"hello"})";
  VariantScalars t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.value.index(), 0u); // string
  EXPECT_EQ(std::get<0>(t.value), "hello");
}

TEST(IdlTool, ParseVariantInt) {
  const char *json = R"({"value":42})";
  VariantScalars t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.value.index(), 1u); // int64
  EXPECT_EQ(std::get<1>(t.value), 42);
}

TEST(IdlTool, ParseVariantBool) {
  const char *json = R"({"value":true})";
  VariantScalars t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.value.index(), 2u); // bool
  EXPECT_EQ(std::get<2>(t.value), true);
}

TEST(IdlTool, SerializeVariantString) {
  VariantScalars t;
  t.value = std::string("world");
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["value"].GetString(), "world");
}

TEST(IdlTool, SerializeVariantInt) {
  VariantScalars t;
  t.value = (int64_t)99;
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_EQ(doc["value"].GetInt64(), 99);
}

TEST(IdlTool, SerializeVariantBool) {
  VariantScalars t;
  t.value = false;
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_EQ(doc["value"].GetBool(), false);
}

TEST(IdlTool, RoundtripVariantScalars) {
  // String
  {
    VariantScalars t;
    t.value = std::string("test");
    xmstream out;
    t.serialize(out);
    VariantScalars t2;
    ASSERT_TRUE(t2.parse(reinterpret_cast<const char*>(out.data()), out.sizeOf()));
    ASSERT_EQ(t2.value.index(), 0u);
    EXPECT_EQ(std::get<0>(t2.value), "test");
  }
  // Int
  {
    VariantScalars t;
    t.value = (int64_t)-100;
    xmstream out;
    t.serialize(out);
    VariantScalars t2;
    ASSERT_TRUE(t2.parse(reinterpret_cast<const char*>(out.data()), out.sizeOf()));
    ASSERT_EQ(t2.value.index(), 1u);
    EXPECT_EQ(std::get<1>(t2.value), -100);
  }
}

TEST(IdlTool, ParseVariantWithStruct) {
  // Struct alternative
  const char *json1 = R"({"data":{"value":"x","count":5}})";
  VariantWithStruct t1;
  ASSERT_TRUE(t1.parse(json1, strlen(json1)));
  ASSERT_EQ(t1.data.index(), 0u); // Inner
  EXPECT_EQ(std::get<0>(t1.data).value, "x");
  EXPECT_EQ(std::get<0>(t1.data).count, 5);

  // String alternative
  const char *json2 = R"({"data":"plaintext"})";
  VariantWithStruct t2;
  ASSERT_TRUE(t2.parse(json2, strlen(json2)));
  ASSERT_EQ(t2.data.index(), 1u); // string
  EXPECT_EQ(std::get<1>(t2.data), "plaintext");
}

TEST(IdlTool, SerializeVariantWithStruct) {
  VariantWithStruct t;
  Inner inner;
  inner.value = "v";
  inner.count = 7;
  t.data = inner;
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["data"].IsObject());
  EXPECT_STREQ(doc["data"]["value"].GetString(), "v");
  EXPECT_EQ(doc["data"]["count"].GetInt64(), 7);
}

TEST(IdlTool, ParseVariantMultiStruct) {
  // Circle (discriminated by "radius" field)
  const char *json1 = R"({"shape":{"radius":5.0}})";
  VariantMultiStruct t1;
  ASSERT_TRUE(t1.parse(json1, strlen(json1)));
  ASSERT_EQ(t1.shape.index(), 0u); // ShapeCircle
  EXPECT_DOUBLE_EQ(std::get<0>(t1.shape).radius, 5.0);

  // Rect (discriminated by "width" or "height" field)
  const char *json2 = R"({"shape":{"width":10.0,"height":20.0}})";
  VariantMultiStruct t2;
  ASSERT_TRUE(t2.parse(json2, strlen(json2)));
  ASSERT_EQ(t2.shape.index(), 1u); // ShapeRect
  EXPECT_DOUBLE_EQ(std::get<1>(t2.shape).width, 10.0);
  EXPECT_DOUBLE_EQ(std::get<1>(t2.shape).height, 20.0);
}

TEST(IdlTool, SerializeVariantMultiStruct) {
  VariantMultiStruct t;
  ShapeCircle c;
  c.radius = 3.0;
  t.shape = c;
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["shape"].IsObject());
  EXPECT_DOUBLE_EQ(doc["shape"]["radius"].GetDouble(), 3.0);
}

TEST(IdlTool, RoundtripVariantMultiStruct) {
  // Circle roundtrip
  {
    VariantMultiStruct t;
    ShapeCircle c;
    c.radius = 7.5;
    t.shape = c;
    xmstream out;
    t.serialize(out);
    VariantMultiStruct t2;
    ASSERT_TRUE(t2.parse(reinterpret_cast<const char*>(out.data()), out.sizeOf()));
    ASSERT_EQ(t2.shape.index(), 0u);
    EXPECT_DOUBLE_EQ(std::get<0>(t2.shape).radius, 7.5);
  }
  // Rect roundtrip
  {
    VariantMultiStruct t;
    ShapeRect r;
    r.width = 4.0;
    r.height = 6.0;
    t.shape = r;
    xmstream out;
    t.serialize(out);
    VariantMultiStruct t2;
    ASSERT_TRUE(t2.parse(reinterpret_cast<const char*>(out.data()), out.sizeOf()));
    ASSERT_EQ(t2.shape.index(), 1u);
    EXPECT_DOUBLE_EQ(std::get<1>(t2.shape).width, 4.0);
    EXPECT_DOUBLE_EQ(std::get<1>(t2.shape).height, 6.0);
  }
}

TEST(IdlTool, ParseOptionalVariantPresent) {
  const char *json = R"({"label":"hi","extra":"bonus"})";
  OptionalVariant t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.label, "hi");
  ASSERT_TRUE(t.extra.has_value());
  ASSERT_EQ(t.extra->index(), 0u); // string
  EXPECT_EQ(std::get<0>(*t.extra), "bonus");
}

TEST(IdlTool, ParseOptionalVariantAbsent) {
  const char *json = R"({"label":"hi"})";
  OptionalVariant t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  EXPECT_EQ(t.label, "hi");
  EXPECT_FALSE(t.extra.has_value());
}

TEST(IdlTool, ParseOptionalVariantInt) {
  const char *json = R"({"label":"hi","extra":123})";
  OptionalVariant t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_TRUE(t.extra.has_value());
  ASSERT_EQ(t.extra->index(), 1u); // int64
  EXPECT_EQ(std::get<1>(*t.extra), 123);
}

TEST(IdlTool, ParseOptionalVariantNullRejected) {
  const char *json = R"({"label":"hi","extra":null})";
  OptionalVariant t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(IdlTool, SerializeOptionalVariantAbsent) {
  OptionalVariant t;
  t.label = "test";
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["label"].GetString(), "test");
  EXPECT_FALSE(doc.HasMember("extra"));
}

TEST(IdlTool, SerializeOptionalVariantPresent) {
  OptionalVariant t;
  t.label = "test";
  t.extra = std::variant<std::string, int64_t>(std::string("val"));
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_STREQ(doc["extra"].GetString(), "val");
}

TEST(IdlTool, ParseVariantMappedString) {
  const char *json = R"({"data":"42"})";
  VariantMappedString t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.data.index(), 0u);
  EXPECT_EQ(std::get<0>(t.data), 42);
}

TEST(IdlTool, SerializeVariantMappedString) {
  VariantMappedString t;
  t.data = int64_t(73);
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["data"].IsString());
  EXPECT_STREQ(doc["data"].GetString(), "73");
}

TEST(IdlTool, ParseVariantMappedUInt) {
  const char *json = R"({"data":1234567890123})";
  VariantMappedUInt t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.data.index(), 0u);
  EXPECT_EQ(std::get<0>(t.data), 1234567890123ULL);
}

TEST(IdlTool, SerializeVariantMappedUInt) {
  VariantMappedUInt t;
  t.data = uint64_t(1234567890123ULL);
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["data"].IsUint64());
  EXPECT_EQ(doc["data"].GetUint64(), 1234567890123ULL);
}

TEST(IdlTool, VerboseParseVariantMappedUInt) {
  const char *json = R"({"data":77})";
  VariantMappedUInt t;
  ParseError err;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), err));
  ASSERT_EQ(t.data.index(), 0u);
  EXPECT_EQ(std::get<0>(t.data), 77ULL);
}

TEST(IdlTool, ParseVariantUInt64OrDoubleNegativeFallsBackToDouble) {
  const char *json = R"({"value":-1})";
  VariantUInt64OrDouble t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.value.index(), 1u);
  EXPECT_DOUBLE_EQ(std::get<1>(t.value), -1.0);
}

TEST(IdlTool, VerboseParseVariantUInt64OrDoubleNegativeFallsBackToDouble) {
  const char *json = R"({"value":-1})";
  VariantUInt64OrDouble t;
  ParseError err;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), err));
  ASSERT_EQ(t.value.index(), 1u);
  EXPECT_DOUBLE_EQ(std::get<1>(t.value), -1.0);
}

TEST(IdlTool, ParseVariantMappedUIntOrDoubleNegativeFallsBackToDouble) {
  const char *json = R"({"value":-1})";
  VariantMappedUIntOrDouble t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.value.index(), 1u);
  EXPECT_DOUBLE_EQ(std::get<1>(t.value), -1.0);
}

TEST(IdlTool, VerboseParseVariantMappedUIntOrDoubleNegativeFallsBackToDouble) {
  const char *json = R"({"value":-1})";
  VariantMappedUIntOrDouble t;
  ParseError err;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), err));
  ASSERT_EQ(t.value.index(), 1u);
  EXPECT_DOUBLE_EQ(std::get<1>(t.value), -1.0);
}

TEST(IdlTool, ParseVariantTensorThenMatrixAmbiguousEmptyPrefersFirst) {
  const char *json = R"({"value":[]})";
  VariantTensorThenMatrix t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.value.index(), 0u);
  EXPECT_TRUE(std::get<0>(t.value).empty());
}

TEST(IdlTool, ParseVariantTensorThenMatrixFallsBackToMatrix) {
  const char *json = R"({"value":[[1.0,2.0],[3.0]]})";
  VariantTensorThenMatrix t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.value.index(), 1u);
  const auto &matrix = std::get<1>(t.value);
  ASSERT_EQ(matrix.size(), 2u);
  ASSERT_EQ(matrix[0].size(), 2u);
  EXPECT_DOUBLE_EQ(matrix[0][0], 1.0);
  EXPECT_DOUBLE_EQ(matrix[0][1], 2.0);
  ASSERT_EQ(matrix[1].size(), 1u);
  EXPECT_DOUBLE_EQ(matrix[1][0], 3.0);
}

TEST(IdlTool, VerboseParseVariantTensorThenMatrixFallsBackToMatrix) {
  const char *json = R"({"value":[[1.0,2.0],[3.0]]})";
  VariantTensorThenMatrix t;
  ParseError err;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), err));
  ASSERT_EQ(t.value.index(), 1u);
  EXPECT_DOUBLE_EQ(std::get<1>(t.value)[0][1], 2.0);
}

TEST(IdlTool, ParseVariantTensorThenMatrixRejectsOneDimensionalArray) {
  const char *json = R"({"value":[1.0]})";
  VariantTensorThenMatrix t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(IdlTool, ParseVariantMatrixThenTensorAmbiguousEmptyPrefersFirst) {
  const char *json = R"({"value":[]})";
  VariantMatrixThenTensor t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.value.index(), 0u);
  EXPECT_TRUE(std::get<0>(t.value).empty());
}

TEST(IdlTool, ParseVariantMatrixThenTensorFallsBackToTensor) {
  const char *json = R"({"value":[[[1.0],[2.0]],[[3.0]]]})";
  VariantMatrixThenTensor t;
  ASSERT_TRUE(t.parse(json, strlen(json)));
  ASSERT_EQ(t.value.index(), 1u);
  const auto &tensor = std::get<1>(t.value);
  ASSERT_EQ(tensor.size(), 2u);
  ASSERT_EQ(tensor[0].size(), 2u);
  ASSERT_EQ(tensor[0][0].size(), 1u);
  EXPECT_DOUBLE_EQ(tensor[0][0][0], 1.0);
  EXPECT_DOUBLE_EQ(tensor[0][1][0], 2.0);
  ASSERT_EQ(tensor[1].size(), 1u);
  EXPECT_DOUBLE_EQ(tensor[1][0][0], 3.0);
}

TEST(IdlTool, VerboseParseVariantMatrixThenTensorFallsBackToTensor) {
  const char *json = R"({"value":[[[1.0]]]})";
  VariantMatrixThenTensor t;
  ParseError err;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), err));
  ASSERT_EQ(t.value.index(), 1u);
  EXPECT_DOUBLE_EQ(std::get<1>(t.value)[0][0][0], 1.0);
}

TEST(IdlTool, ParseVariantMatrixThenTensorRejectsOneDimensionalArray) {
  const char *json = R"({"value":[1.0]})";
  VariantMatrixThenTensor t;
  EXPECT_FALSE(t.parse(json, strlen(json)));
}

TEST(IdlTool, SerializeVariantTensorThenMatrixFirstAlternative) {
  VariantTensorThenMatrix t;
  std::vector<std::vector<std::vector<double>>> tensor = {{{1.0, 2.0}}, {{3.0}}};
  t.value = tensor;
  xmstream out;
  t.serialize(out);
  auto doc = parseRapid(out);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["value"].IsArray());
  ASSERT_EQ(doc["value"].Size(), 2u);
  ASSERT_EQ(doc["value"][0].Size(), 1u);
  ASSERT_EQ(doc["value"][0][0].Size(), 2u);
  EXPECT_DOUBLE_EQ(doc["value"][0][0][0].GetDouble(), 1.0);
  EXPECT_DOUBLE_EQ(doc["value"][1][0][0].GetDouble(), 3.0);
}

TEST(IdlTool, RoundtripVariantMatrixThenTensorTensorAlternative) {
  VariantMatrixThenTensor t;
  std::vector<std::vector<std::vector<double>>> tensor = {{{4.0}}, {{5.0, 6.0}}};
  t.value = tensor;
  xmstream out;
  t.serialize(out);
  VariantMatrixThenTensor t2;
  ASSERT_TRUE(t2.parse(reinterpret_cast<const char*>(out.data()), out.sizeOf()));
  ASSERT_EQ(t2.value.index(), 1u);
  EXPECT_DOUBLE_EQ(std::get<1>(t2.value)[0][0][0], 4.0);
  EXPECT_DOUBLE_EQ(std::get<1>(t2.value)[1][0][1], 6.0);
}

TEST(IdlTool, VerboseParseVariantString) {
  const char *json = R"({"value":"hello"})";
  VariantScalars t;
  ParseError err;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), err));
  ASSERT_EQ(t.value.index(), 0u);
  EXPECT_EQ(std::get<0>(t.value), "hello");
}

TEST(IdlTool, VerboseParseVariantInt) {
  const char *json = R"({"value":42})";
  VariantScalars t;
  ParseError err;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), err));
  ASSERT_EQ(t.value.index(), 1u);
  EXPECT_EQ(std::get<1>(t.value), 42);
}

TEST(IdlTool, VerboseParseVariantMultiStruct) {
  const char *json = R"({"shape":{"radius":5.0}})";
  VariantMultiStruct t;
  ParseError err;
  ASSERT_TRUE(t.parseVerbose(json, strlen(json), err));
  ASSERT_EQ(t.shape.index(), 0u);
  EXPECT_DOUBLE_EQ(std::get<0>(t.shape).radius, 5.0);
}

// ============================================================================
// Optional syntax regression tests
// ============================================================================

TEST(IdlTool, LegacyOptionalSyntaxRejected) {
  const std::string idl =
    "struct Sample {\n"
    "  .generate(parse);\n"
    "  value: string?;\n"
    "}\n";

  EXPECT_TRUE(generatedIdlFails("legacy_optional_syntax", idl));
}

TEST(IdlTool, OptionalUnknownPolicyRejected) {
  const std::string idl =
    "struct Sample {\n"
    "  .generate(parse);\n"
    "  value: optional<string>(wat=allow);\n"
    "}\n";

  EXPECT_TRUE(generatedIdlFails("optional_unknown_policy", idl, "unknown optional<T> policy"));
}

TEST(IdlTool, OptionalInvalidNullInRejected) {
  const std::string idl =
    "struct Sample {\n"
    "  .generate(parse);\n"
    "  value: optional<string>(null_in=maybe);\n"
    "}\n";

  EXPECT_TRUE(generatedIdlFails("optional_invalid_null_in", idl, "invalid value for null_in"));
}

TEST(IdlTool, OptionalDuplicatePolicyRejected) {
  const std::string idl =
    "struct Sample {\n"
    "  .generate(parse);\n"
    "  value: optional<string>(null_in=allow, null_in=required);\n"
    "}\n";

  EXPECT_TRUE(generatedIdlFails("optional_duplicate_policy", idl, "duplicate optional<T> policy"));
}

// ============================================================================
// Default type mismatch validation
// ============================================================================

TEST(IdlTool, DefaultIntOnStringRejected) {
  EXPECT_TRUE(generatedIdlFails("default_int_on_string",
    "struct Bad { .generate(parse); value: string = 1; }\n",
    "incompatible default"));
}

TEST(IdlTool, DefaultStringOnIntRejected) {
  EXPECT_TRUE(generatedIdlFails("default_string_on_int",
    "struct Bad { .generate(parse); value: int32 = \"hello\"; }\n",
    "incompatible default"));
}

TEST(IdlTool, DefaultBoolOnIntRejected) {
  EXPECT_TRUE(generatedIdlFails("default_bool_on_int",
    "struct Bad { .generate(parse); value: int32 = true; }\n",
    "incompatible default"));
}

TEST(IdlTool, DefaultIntOnBoolRejected) {
  EXPECT_TRUE(generatedIdlFails("default_int_on_bool",
    "struct Bad { .generate(parse); value: bool = 0; }\n",
    "incompatible default"));
}

TEST(IdlTool, DefaultStringOnDoubleRejected) {
  EXPECT_TRUE(generatedIdlFails("default_string_on_double",
    "struct Bad { .generate(parse); value: double = \"nope\"; }\n",
    "incompatible default"));
}

TEST(IdlTool, DefaultFloatOnStringRejected) {
  EXPECT_TRUE(generatedIdlFails("default_float_on_string",
    "struct Bad { .generate(parse); value: string = 3.14; }\n",
    "incompatible default"));
}

TEST(IdlTool, DefaultStringOnBoolRejected) {
  EXPECT_TRUE(generatedIdlFails("default_string_on_bool",
    "struct Bad { .generate(parse); value: bool = \"yes\"; }\n",
    "incompatible default"));
}

TEST(IdlTool, DefaultBoolOnDoubleRejected) {
  EXPECT_TRUE(generatedIdlFails("default_bool_on_double",
    "struct Bad { .generate(parse); value: double = false; }\n",
    "incompatible default"));
}

TEST(IdlTool, DefaultFloatOnIntRejected) {
  EXPECT_TRUE(generatedIdlFails("default_float_on_int",
    "struct Bad { .generate(parse); value: int64 = 3.14; }\n",
    "incompatible default"));
}

TEST(IdlTool, DefaultIntOnDoubleAllowed) {
  // Int literal on double field is reasonable (3 → 3.0)
  EXPECT_FALSE(generatedIdlFails("default_int_on_double",
    "struct OK { .generate(parse); value: double = 3; }\n"));
}

TEST(IdlTool, DefaultFloatOnChronoRejected) {
  EXPECT_TRUE(generatedIdlFails("default_float_on_chrono",
    "struct Bad { .generate(parse); value: chrono::seconds = 3.14; }\n",
    "incompatible default"));
}

// ============================================================================
// Schema parser regression tests
// ============================================================================

TEST(IdlTool, LegacyContextDirectiveRejected) {
  const std::string idl =
    "mapped type testVal(\"int64_t\") : string context(uint32) include \"support.h\";\n"
    "struct Parent {\n"
    "  .generate(parse, serialize);\n"
    "  .context(testVal);\n"
    "  value: testVal;\n"
    "}\n";

  EXPECT_TRUE(generatedIdlFails("legacy_context_directive", idl));
}

// ============================================================================
// Bool/null word boundary tests
// ============================================================================

TEST(IdlTool, LiteralWordBoundary) {
  // --- reject: no word boundary after bool/null literals ---

  // "trueX"
  {
    const char *json = R"({"fieldString":"a","fieldBool":trueX,"fieldInt32":0,"fieldUint32":0,"fieldInt64":0,"fieldUint64":0,"fieldDouble":0})";
    ScalarTypes t;
    EXPECT_FALSE(t.parse(json, strlen(json)));
  }
  // "falsehood"
  {
    const char *json = R"({"fieldString":"a","fieldBool":falsehood,"fieldInt32":0,"fieldUint32":0,"fieldInt64":0,"fieldUint64":0,"fieldDouble":0})";
    ScalarTypes t;
    EXPECT_FALSE(t.parse(json, strlen(json)));
  }
  // "true1"
  {
    const char *json = R"({"fieldString":"a","fieldBool":true1,"fieldInt32":0,"fieldUint32":0,"fieldInt64":0,"fieldUint64":0,"fieldDouble":0})";
    ScalarTypes t;
    EXPECT_FALSE(t.parse(json, strlen(json)));
  }
  // "nullify" for nullable scalar
  {
    const char *json = R"({"title":"a","note":nullify})";
    NullableScalars s;
    EXPECT_FALSE(s.parse(json, strlen(json)));
  }
  // "nullX" for nullable object
  {
    const char *json = R"({"title":"a","inner":nullX})";
    NullableObject s;
    EXPECT_FALSE(s.parse(json, strlen(json)));
  }
  // "null1"
  {
    const char *json = R"({"title":"a","note":null1})";
    NullableScalars s;
    EXPECT_FALSE(s.parse(json, strlen(json)));
  }
  // variant(string, int64, bool) with "trueish"
  {
    const char *json = R"({"value":trueish})";
    VariantScalars v;
    EXPECT_FALSE(v.parse(json, strlen(json)));
  }
  // verbose: bool
  {
    const char *json = R"({"fieldString":"a","fieldBool":trueX,"fieldInt32":0,"fieldUint32":0,"fieldInt64":0,"fieldUint64":0,"fieldDouble":0})";
    ScalarTypes t;
    ParseError error;
    EXPECT_FALSE(t.parseVerbose(json, strlen(json), error));
    EXPECT_FALSE(error.message.empty());
  }
  // verbose: null
  {
    const char *json = R"({"title":"a","note":nullify})";
    NullableScalars s;
    ParseError error;
    EXPECT_FALSE(s.parseVerbose(json, strlen(json), error));
    EXPECT_FALSE(error.message.empty());
  }

  // --- accept: valid boundaries (comma, }, whitespace, end-of-input) ---

  {
    const char *json = R"({"required1":"a","required2":1,"required3":true,"optional3":false})";
    MixedFields m;
    ASSERT_TRUE(m.parse(json, strlen(json)));
    EXPECT_TRUE(m.required3);
    EXPECT_FALSE(m.optional3);
  }
  {
    const char *json = R"({"required1":"a","required2":1,"required3":true})";
    MixedFields m;
    ASSERT_TRUE(m.parse(json, strlen(json)));
    EXPECT_TRUE(m.required3);
  }
  {
    const char *json = R"({"required1":"a","required2":1,"required3":true })";
    MixedFields m;
    ASSERT_TRUE(m.parse(json, strlen(json)));
    EXPECT_TRUE(m.required3);
  }
  {
    const char *json = R"({"title":"a","note":null,"numbers":null})";
    NullableScalars s;
    ASSERT_TRUE(s.parse(json, strlen(json)));
    EXPECT_FALSE(s.note.has_value());
  }
  {
    const char *json = R"({"title":"a","note":null})";
    NullableScalars s;
    ASSERT_TRUE(s.parse(json, strlen(json)));
    EXPECT_FALSE(s.note.has_value());
  }
  {
    const char *json = R"({"title":"a","note":null ,"numbers":null})";
    NullableScalars s;
    ASSERT_TRUE(s.parse(json, strlen(json)));
    EXPECT_FALSE(s.note.has_value());
  }
}

// ============================================================================
// Negative float defaults
// ============================================================================

TEST(IdlTool, ParseNegativeFloatDefault) {
  ScalarDefaults d;
  const char *json = R"({})";
  ASSERT_TRUE(d.parse(json, strlen(json)));
  EXPECT_DOUBLE_EQ(d.fieldNegDouble, -3.14);
}

TEST(IdlTool, ParseNegativeFloatDefaultOverride) {
  const char *json = R"({"fieldNegDouble":-99.5})";
  ScalarDefaults d;
  ASSERT_TRUE(d.parse(json, strlen(json)));
  EXPECT_DOUBLE_EQ(d.fieldNegDouble, -99.5);
}

TEST(IdlTool, SerializeNegativeFloatDefault) {
  ScalarDefaults d;
  d.fieldNegDouble = -2.718;

  xmstream stream;
  d.serialize(stream);
  auto doc = parseRapid(stream);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.HasMember("fieldNegDouble"));
  EXPECT_DOUBLE_EQ(doc["fieldNegDouble"].GetDouble(), -2.718);
}

// ============================================================================
// Duplicate definition validation
// ============================================================================

TEST(IdlTool, DuplicateStructRejected) {
  EXPECT_TRUE(generatedIdlFails("dup_struct", R"(
struct Foo { .generate(parse); x: int32; }
struct Foo { .generate(parse); y: int32; }
)", "duplicate"));
}

TEST(IdlTool, DuplicateEnumRejected) {
  EXPECT_TRUE(generatedIdlFails("dup_enum", R"(
enum Color : string { red, green, blue }
enum Color : string { cyan, magenta }
struct S { .generate(parse); c: Color; }
)", "duplicate"));
}

TEST(IdlTool, DuplicateMappedTypeRejected) {
  EXPECT_TRUE(generatedIdlFails("dup_mapped", R"(
mapped type money("int64_t") : string include "support.h";
mapped type money("double") : string include "support.h";
struct S { .generate(parse); v: money; }
)", "duplicate"));
}

TEST(IdlTool, DuplicateEnumValueRejected) {
  EXPECT_TRUE(generatedIdlFails("dup_enum_val", R"(
enum Color : string { red, green, red }
struct S { .generate(parse); c: Color; }
)", "duplicate"));
}

TEST(IdlTool, NameCollisionStructEnumRejected) {
  EXPECT_TRUE(generatedIdlFails("name_clash_struct_enum", R"(
struct Foo { .generate(parse); x: int32; }
enum Foo : string { a, b }
)", "duplicate"));
}

TEST(IdlTool, NameCollisionStructMappedRejected) {
  EXPECT_TRUE(generatedIdlFails("name_clash_struct_mapped", R"(
struct Foo { .generate(parse); x: int32; }
mapped type Foo("int64_t") : string include "support.h";
)", "duplicate"));
}

TEST(IdlTool, NameCollisionEnumMappedRejected) {
  EXPECT_TRUE(generatedIdlFails("name_clash_enum_mapped", R"(
enum Foo : string { a, b }
mapped type Foo("int64_t") : string include "support.h";
struct S { .generate(parse); x: int32; }
)", "duplicate"));
}

// ============================================================================
// Include cycle: root file self-include
// ============================================================================

TEST(IdlTool, SelfIncludeSilentlyIgnored) {
  // A file that includes itself should not cause duplicated types or errors.
  // The root file is seeded into visited, so the self-include is skipped.
  try {
    TempDir tempDir(makeTempDir());
    auto idlPath = tempDir.path() / "self.idl";
    auto headerPath = tempDir.path() / "self.idl.h";
    auto genLogPath = tempDir.path() / "idltool.log";

    writeTextFile(tempDir.path() / "support.h", "#pragma once\n");
    writeTextFile(idlPath,
      "include \"self.idl\";\n"
      "struct Foo { .generate(parse); x: int32; }\n");

    std::string generateCmd =
      shellQuote(IDLTOOL_BINARY_PATH) + " " +
      shellQuote(idlPath.string()) + " -o " +
      shellQuote(headerPath.string());
    auto genResult = runCommandCapture(generateCmd, genLogPath);
    EXPECT_EQ(genResult.ExitCode, 0)
      << "idltool failed for self-include:\n" << genResult.Output;
  } catch (const std::exception &e) {
    FAIL() << e.what();
  }
}

// ============================================================================
// String default C++ escaping
// ============================================================================

TEST(IdlTool, StringDefaultWithSpecialCharsEscaped) {
  // The IDL lexer preserves escape sequences raw in StringVal.
  // cppStringLiteral() must re-escape them for valid C++.
  // IDL "hello\"world" → StringVal = hello\"world → C++ "hello\\\"world"
  // IDL "back\\slash"  → StringVal = back\\slash  → C++ "back\\\\slash"
  EXPECT_TRUE(generatedIdlHeaderContains("str_escape_default",
    R"(
struct Escaped {
  .generate(parse, serialize);
  a: string = "hello\"world";
  b: string = "back\\slash";
}
)", {R"("hello\\\"world")", R"("back\\\\slash")"}));
}

// ============================================================================
// Tagged schema with serialize.flat only must still emit fields
// ============================================================================

TEST(IdlTool, TaggedSchemaFlatOnlyEmitsFields) {
  // A struct with only .generate(serialize.flat) and tagged fields
  // must still emit member declarations in the header, because
  // schema() references &Struct::member.
  EXPECT_TRUE(generatedIdlHeaderContains("tagged_flat_only",
    "struct Tagged {\n"
    "  .generate(serialize.flat);\n"
    "  x: int32 @1;\n"
    "  y: string @2;\n"
    "}\n",
    {"int32_t x", "std::string y", "schema()"}));
}

// ============================================================================
// Map fields
// ============================================================================

TEST(IdlTool, ParseMapFields) {
  const char *json = R"({
    "label":"test",
    "scalarMap":{"a":1.5,"b":2.5,"c":3.5},
    "objectMap":{"x":{"value":"hello","count":10},"y":{"value":"world","count":20}},
    "optionalScalarMap":{"k1":100,"k2":200},
    "optionalObjectMap":{"p":{"value":"v","count":1}}
  })";
  MapFields mf;
  ASSERT_TRUE(mf.parse(json, strlen(json)));

  EXPECT_EQ(mf.label, "test");

  ASSERT_EQ(mf.scalarMap.size(), 3u);
  EXPECT_DOUBLE_EQ(mf.scalarMap.at("a"), 1.5);
  EXPECT_DOUBLE_EQ(mf.scalarMap.at("b"), 2.5);
  EXPECT_DOUBLE_EQ(mf.scalarMap.at("c"), 3.5);

  ASSERT_EQ(mf.objectMap.size(), 2u);
  EXPECT_EQ(mf.objectMap.at("x").value, "hello");
  EXPECT_EQ(mf.objectMap.at("x").count, 10);
  EXPECT_EQ(mf.objectMap.at("y").value, "world");
  EXPECT_EQ(mf.objectMap.at("y").count, 20);

  ASSERT_TRUE(mf.optionalScalarMap.has_value());
  ASSERT_EQ(mf.optionalScalarMap->size(), 2u);
  EXPECT_EQ(mf.optionalScalarMap->at("k1"), 100);
  EXPECT_EQ(mf.optionalScalarMap->at("k2"), 200);

  ASSERT_TRUE(mf.optionalObjectMap.has_value());
  ASSERT_EQ(mf.optionalObjectMap->size(), 1u);
  EXPECT_EQ(mf.optionalObjectMap->at("p").value, "v");
  EXPECT_EQ(mf.optionalObjectMap->at("p").count, 1);
}

TEST(IdlTool, ParseMapOptionalAbsent) {
  const char *json = R"({"label":"x","scalarMap":{},"objectMap":{}})";
  MapFields mf;
  ASSERT_TRUE(mf.parse(json, strlen(json)));
  EXPECT_EQ(mf.label, "x");
  EXPECT_TRUE(mf.scalarMap.empty());
  EXPECT_TRUE(mf.objectMap.empty());
  EXPECT_FALSE(mf.optionalScalarMap.has_value());
  EXPECT_FALSE(mf.optionalObjectMap.has_value());
}

TEST(IdlTool, ParseMapEmptyObjects) {
  const char *json = R"({"label":"e","scalarMap":{},"objectMap":{},"optionalScalarMap":{},"optionalObjectMap":{}})";
  MapFields mf;
  ASSERT_TRUE(mf.parse(json, strlen(json)));
  EXPECT_TRUE(mf.scalarMap.empty());
  EXPECT_TRUE(mf.objectMap.empty());
  ASSERT_TRUE(mf.optionalScalarMap.has_value());
  EXPECT_TRUE(mf.optionalScalarMap->empty());
  ASSERT_TRUE(mf.optionalObjectMap.has_value());
  EXPECT_TRUE(mf.optionalObjectMap->empty());
}

TEST(IdlTool, ParseMapMissingRequired) {
  const char *json = R"({"label":"x","objectMap":{}})";
  MapFields mf;
  EXPECT_FALSE(mf.parse(json, strlen(json)));
}

TEST(IdlTool, SerializeMapRoundtrip) {
  MapFields mf;
  mf.label = "rt";
  mf.scalarMap["alpha"] = 1.0;
  mf.scalarMap["beta"] = 2.0;
  mf.objectMap["item1"] = Inner{"val1", 11};
  mf.optionalScalarMap.emplace();
  (*mf.optionalScalarMap)["k"] = 42;

  xmstream stream;
  mf.serialize(stream);
  std::string serialized(reinterpret_cast<const char*>(stream.data()), stream.sizeOf());

  MapFields mf2;
  ASSERT_TRUE(mf2.parse(serialized.data(), serialized.size()));
  EXPECT_EQ(mf2.label, "rt");
  EXPECT_EQ(mf2.scalarMap.size(), 2u);
  EXPECT_DOUBLE_EQ(mf2.scalarMap.at("alpha"), 1.0);
  EXPECT_DOUBLE_EQ(mf2.scalarMap.at("beta"), 2.0);
  ASSERT_EQ(mf2.objectMap.size(), 1u);
  EXPECT_EQ(mf2.objectMap.at("item1").value, "val1");
  EXPECT_EQ(mf2.objectMap.at("item1").count, 11);
  ASSERT_TRUE(mf2.optionalScalarMap.has_value());
  EXPECT_EQ(mf2.optionalScalarMap->at("k"), 42);
  EXPECT_FALSE(mf2.optionalObjectMap.has_value());
}

TEST(IdlTool, ParseMapVerbose) {
  const char *json = R"({"label":"v","scalarMap":{"x":1.0},"objectMap":{"y":{"value":"a","count":1}}})";
  MapFields mf;
  ParseError error;
  ASSERT_TRUE(mf.parseVerbose(json, strlen(json), error));
  EXPECT_EQ(mf.scalarMap.at("x"), 1.0);
  EXPECT_EQ(mf.objectMap.at("y").value, "a");
}

TEST(IdlTool, ParseMapVerboseError) {
  const char *json = R"({"label":"v","scalarMap":{"x":"not_a_number"},"objectMap":{}})";
  MapFields mf;
  ParseError error;
  EXPECT_FALSE(mf.parseVerbose(json, strlen(json), error));
}

