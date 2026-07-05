// test_serialize.cpp — the JSON serializer: parse → serialize → parse deep-
// equality, byte-for-byte key-order preservation on a curated sample, string
// escaping, and shortest round-trip number formatting.
#include "test_harness.h"

#include "json.h"

#include <string>

using namespace cv;

namespace {

bool parses(const std::string& text, JsonValue& out) {
    std::string err;
    return JsonParser::parse(text, out, err);
}

// Structural deep equality (numbers compared exactly — the serializer must
// round-trip doubles bit-exactly).
bool deepEqual(const JsonValue& a, const JsonValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case JsonValue::Type::Null:   return true;
        case JsonValue::Type::Bool:   return a.boolVal == b.boolVal;
        case JsonValue::Type::Number: return a.numVal == b.numVal;
        case JsonValue::Type::String: return a.strVal == b.strVal;
        case JsonValue::Type::Array:
            if (a.arr.size() != b.arr.size()) return false;
            for (size_t i = 0; i < a.arr.size(); ++i)
                if (!deepEqual(a.arr[i], b.arr[i])) return false;
            return true;
        case JsonValue::Type::Object:
            if (a.obj.size() != b.obj.size()) return false;
            for (size_t i = 0; i < a.obj.size(); ++i) {   // order matters
                if (a.obj[i].first != b.obj[i].first) return false;
                if (!deepEqual(a.obj[i].second, b.obj[i].second)) return false;
            }
            return true;
    }
    return false;
}

// serialize(parse(text)) parses back deep-equal to the original.
bool roundTrips(const std::string& text) {
    JsonValue v1, v2;
    if (!parses(text, v1)) return false;
    if (!parses(serialize(v1), v2)) return false;
    return deepEqual(v1, v2);
}

} // namespace

void test_serialize() {
    // --- Scalars --------------------------------------------------------------
    {
        JsonValue v;
        CHECK(parses("true", v)  && serialize(v) == "true");
        CHECK(parses("false", v) && serialize(v) == "false");
        CHECK(parses("null", v)  && serialize(v) == "null");
        CHECK(parses("\"hi\"", v) && serialize(v) == "\"hi\"");
        CHECK(parses("{}", v) && serialize(v) == "{}");
        CHECK(parses("[]", v) && serialize(v) == "[]");
    }

    // --- Numbers: shortest representation that round-trips ---------------------
    {
        JsonValue v;
        CHECK(parses("1", v)      && serialize(v) == "1");        // no ".0"
        CHECK(parses("-2.5", v)   && serialize(v) == "-2.5");
        CHECK(parses("0.1", v)    && serialize(v) == "0.1");      // not 0.1000...
        CHECK(parses("250", v)    && serialize(v) == "250");
        CHECK(parses("-1.5707963", v) && serialize(v) == "-1.5707963");
        CHECK(parses("1e30", v)   && serialize(v) == "1e+30");
        // A value needing full precision still round-trips exactly.
        JsonValue pi;
        CHECK(parses("3.141592653589793", pi));
        JsonValue back;
        CHECK(parses(serialize(pi), back));
        CHECK(back.numVal == pi.numVal);
    }

    // --- String escaping -------------------------------------------------------
    {
        JsonValue v;
        v.type = JsonValue::Type::String;
        v.strVal = "quote:\" back:\\ nl:\n tab:\t cr:\r bell:\x01";
        std::string s = serialize(v);
        CHECK(s == "\"quote:\\\" back:\\\\ nl:\\n tab:\\t cr:\\r bell:\\u0001\"");
        JsonValue back;
        CHECK(parses(s, back) && back.strVal == v.strVal);
    }

    // --- parse → serialize → parse deep-equal ----------------------------------
    CHECK(roundTrips(R"({
        "name": "cl\"are\n",
        "nested": { "arr": [1, -2.5, 3e2, 0.125e-1, true, false, null, "x"] },
        "empty_obj": {},
        "empty_arr": []
    })"));
    CHECK(roundTrips(R"([{"a": [[1], [2, [3]]]}, 0.30000000000000004, -0])"));

    // Every checked-in scene file round-trips.
    for (const char* path : { "src/levels/demo.json", "src/levels/controls.json",
                              "src/levels/menu.json", "src/levels/world.json" }) {
        std::string text;
        CHECK_MSG(readFile(path, text), path);
        CHECK_MSG(roundTrips(text), path);
    }

    // --- Key order preserved byte-for-byte on a curated sample -----------------
    {
        const std::string curated =
            "{\n"
            "  \"version\": 1,\n"
            "  \"zeta\": true,\n"
            "  \"alpha\": {\n"
            "    \"pos\": [\n"
            "      1,\n"
            "      0.5,\n"
            "      -3\n"
            "    ],\n"
            "    \"name\": \"mochi\"\n"
            "  },\n"
            "  \"mid\": [\n"
            "    {\n"
            "      \"b\": 2,\n"
            "      \"a\": 1\n"
            "    }\n"
            "  ]\n"
            "}";
        JsonValue v;
        CHECK(parses(curated, v));
        CHECK(serialize(v) == curated);           // byte-for-byte, order intact
        // And again through a second cycle: serializer output is a fixed point.
        JsonValue v2;
        CHECK(parses(serialize(v), v2));
        CHECK(serialize(v2) == curated);
    }

    // --- Custom indent width -----------------------------------------------------
    {
        JsonValue v;
        CHECK(parses(R"({"a": [1]})", v));
        CHECK(serialize(v, 4) == "{\n    \"a\": [\n        1\n    ]\n}");
    }
}
