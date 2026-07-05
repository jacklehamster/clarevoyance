// test_json.cpp — the minimal JSON parser: valid documents parse correctly,
// malformed documents fail cleanly, and object key order is preserved.
#include "test_harness.h"

#include "json.h"

#include <cmath>
#include <string>

using namespace cv;

static bool parses(const std::string& text, JsonValue& out) {
    std::string err;
    return JsonParser::parse(text, out, err);
}

static bool fails(const std::string& text) {
    JsonValue out;
    std::string err;
    bool ok = JsonParser::parse(text, out, err);
    // A clean failure reports false AND fills the error string.
    return !ok && !err.empty();
}

void test_json() {
    // --- Valid parse: nesting, escapes, numbers ------------------------------
    {
        JsonValue v;
        CHECK(parses(R"({
            "name": "cl\"are\n",
            "nested": { "arr": [1, -2.5, 3e2, 0.125e-1, true, false, null, "x"] },
            "empty_obj": {},
            "empty_arr": []
        })", v));
        CHECK(v.isObject());
        CHECK(v.find("name") && v.find("name")->string() == "cl\"are\n");
        const JsonValue* nested = v.find("nested");
        CHECK(nested && nested->isObject());
        const JsonValue* arr = nested ? nested->find("arr") : nullptr;
        CHECK(arr && arr->isArray() && arr->arr.size() == 8);
        if (arr && arr->arr.size() == 8) {
            CHECK(arr->arr[0].number() == 1.0);
            CHECK(arr->arr[1].number() == -2.5);
            CHECK(arr->arr[2].number() == 300.0);
            CHECK(std::fabs(arr->arr[3].number() - 0.0125) < 1e-12);
            CHECK(arr->arr[4].boolean() == true);
            CHECK(arr->arr[5].boolean() == false);
            CHECK(arr->arr[6].type == JsonValue::Type::Null);
            CHECK(arr->arr[7].string() == "x");
        }
        CHECK(v.find("empty_obj") && v.find("empty_obj")->isObject()
              && v.find("empty_obj")->obj.empty());
        CHECK(v.find("empty_arr") && v.find("empty_arr")->isArray()
              && v.find("empty_arr")->arr.empty());
    }

    // Escape set: \" \\ \/ \n \t \r \b \f
    {
        JsonValue v;
        CHECK(parses(R"(["\"\\\/\n\t\r\b\f"])", v));
        CHECK(v.isArray() && v.arr.size() == 1);
        if (v.arr.size() == 1)
            CHECK(v.arr[0].string() == "\"\\/\n\t\r\b\f");
    }

    // \uXXXX escapes: ASCII, a control char, and 2- / 3-byte UTF-8 encodings.
    {
        JsonValue v;
        CHECK(parses("[\"\\u0041\\u0001\\u00e9\\u20ac\"]", v));
        CHECK(v.isArray() && v.arr.size() == 1);
        if (v.arr.size() == 1)
            CHECK(v.arr[0].string() == "A\x01\xc3\xa9\xe2\x82\xac");  // A, SOH, e-acute, euro
    }
    CHECK_MSG(fails("[\"\\u12\"]"),   "truncated \\u escape");
    CHECK_MSG(fails("[\"\\u12zx\"]"), "non-hex \\u escape");

    // Bare scalars are valid JSON documents.
    {
        JsonValue v;
        CHECK(parses("-12.75", v));
        CHECK(v.number() == -12.75);
        CHECK(parses("\"hello\"", v));
        CHECK(v.string() == "hello");
        CHECK(parses("true", v));
        CHECK(v.boolean());
    }

    // --- Malformed inputs must fail cleanly ----------------------------------
    CHECK_MSG(fails(""),                "empty input");
    CHECK_MSG(fails("{\"a\": 1"),       "truncated object");
    CHECK_MSG(fails("[1, 2"),           "truncated array");
    CHECK_MSG(fails("\"unterminated"),  "unterminated string");
    CHECK_MSG(fails("1.2.3"),           "double decimal point");
    CHECK_MSG(fails("-"),               "lone minus");
    CHECK_MSG(fails("--5"),             "double minus");
    CHECK_MSG(fails("1e"),              "exponent without digits");
    CHECK_MSG(fails("\"bad \\x esc\""), "bad escape");
    CHECK_MSG(fails("{} garbage"),      "trailing garbage");
    CHECK_MSG(fails("[1] 2"),           "trailing token");
    CHECK_MSG(fails("{a: 1}"),          "unquoted key");
    CHECK_MSG(fails("{\"a\" 1}"),       "missing colon");
    CHECK_MSG(fails("tru"),             "bad literal");

    // --- Key order preserved --------------------------------------------------
    {
        JsonValue v;
        CHECK(parses(R"({"zeta": 1, "alpha": 2, "mid": 3})", v));
        CHECK(v.isObject() && v.obj.size() == 3);
        if (v.obj.size() == 3) {
            CHECK(v.obj[0].first == "zeta");
            CHECK(v.obj[1].first == "alpha");
            CHECK(v.obj[2].first == "mid");
        }
    }
}
