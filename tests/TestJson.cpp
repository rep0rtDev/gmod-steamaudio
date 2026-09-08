// tests/TestJson.cpp
#include "TestFramework.h"
#include "util/Json.h"

using namespace sa;

SA_TEST(Json_ParsesAllValueTypes)
{
    const auto r = ParseJson(R"({
        // comment
        "b": true, "n": -12.5e1, "s": "a\"b\\c\u0041\n",
        "arr": [1, 2, 3], /* block */ "obj": {"k": null}
    })");
    SA_CHECK(r.ok);
    const JsonValue& v = r.value;
    SA_CHECK(v.IsObject());
    SA_CHECK(v["b"].AsBool() == true);
    SA_CHECK_NEAR(v["n"].AsNumber(), -125.0, 1e-9);
    SA_CHECK(v["s"].AsString() == "a\"b\\cA\n");
    SA_CHECK_EQ(v["arr"].Size(), size_t(3));
    SA_CHECK_EQ(v["arr"].At(2).AsInt(), 3);
    SA_CHECK(v["obj"]["k"].IsNull());
    SA_CHECK(v["missing"].IsNull());
    SA_CHECK(v["arr"].At(99).IsNull());
    SA_CHECK(v["missing"].AsString("fallback") == "fallback");
}

SA_TEST(Json_ReportsErrorsWithPosition)
{
    const auto r = ParseJson("{\n  \"a\": [1, 2,\n}");
    SA_CHECK(!r.ok);
    SA_CHECK(!r.error.empty());
    SA_CHECK(r.line >= 2);
}

SA_TEST(Json_RejectsTrailingGarbage)
{
    SA_CHECK(!ParseJson("{} x").ok);
    SA_CHECK(ParseJson("  {}  ").ok);
    SA_CHECK(!ParseJson("").ok);
}

SA_TEST(Json_RejectsNumericOverflowAndExcessiveDepth)
{
    SA_CHECK(!ParseJson("1e309").ok);
    SA_CHECK(!ParseJson(std::string(100, '[') + "0" + std::string(100, ']')).ok);
    SA_CHECK_EQ(JsonValue::MakeNumber(1e30).AsInt(7), 7);
    SA_CHECK_EQ(JsonValue::MakeString("999999999999999999999999").AsInt(7), 7);
    SA_CHECK_NEAR(JsonValue::MakeString("nan").AsNumber(2.0), 2.0, 1e-6);
}

SA_TEST(Json_IntegerConversion)
{
    const auto r = ParseJson(R"({"y": 42, "z": "12", "s": "abc"})");
    SA_CHECK(r.ok);
    SA_CHECK_EQ(r.value["y"].AsInt(), 42);
    SA_CHECK_EQ(r.value["z"].AsInt(7), 12); // numeric strings are accepted (config convenience)
    SA_CHECK_EQ(r.value["s"].AsInt(7), 7);
    SA_CHECK_EQ(r.value["missing"].AsInt(-1), -1);
}
