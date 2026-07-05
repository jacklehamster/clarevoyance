// json.h — a minimal, dependency-free JSON parser + serializer for the game
// data layer.
//
// Scope: just enough JSON to load and write scene/event data files. Supports
// objects, arrays, strings, numbers, true/false/null. Not a general-purpose
// library — no streaming, no unicode escapes beyond the common ones, no
// comments.
//
// Header-only. Parsing is deterministic and allocation-simple; object members
// keep insertion order (a small vector, not a hash map) so parse → serialize
// round-trips preserve key order — hand edits survive an editor pass and
// diffs stay reviewable (MAP_EDITOR spec).
#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cstdlib>
#include <cstdio>

namespace cv {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolVal = false;
    double numVal = 0.0;
    std::string strVal;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    bool isObject() const { return type == Type::Object; }
    bool isArray()  const { return type == Type::Array; }

    // Object lookup. Returns nullptr if absent or not an object.
    const JsonValue* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }

    // Typed getters with defaults — keep call sites terse and crash-free.
    double number(double fallback = 0.0) const {
        return type == Type::Number ? numVal : fallback;
    }
    bool boolean(bool fallback = false) const {
        return type == Type::Bool ? boolVal : fallback;
    }
    std::string string(const std::string& fallback = "") const {
        return type == Type::String ? strVal : fallback;
    }
    float at(size_t i, float fallback = 0.0f) const {
        if (type != Type::Array || i >= arr.size()) return fallback;
        return static_cast<float>(arr[i].number(fallback));
    }
};

// --- Parser ------------------------------------------------------------------

class JsonParser {
public:
    // Returns true on success. On failure, `error` describes where it stopped.
    static bool parse(const std::string& text, JsonValue& out, std::string& error) {
        JsonParser p(text);
        p.skipWs();
        if (!p.parseValue(out)) { error = p.error_; return false; }
        p.skipWs();
        if (p.pos_ != p.text_.size()) { error = "trailing characters after JSON"; return false; }
        return true;
    }

private:
    explicit JsonParser(const std::string& text) : text_(text) {}

    const std::string& text_;
    size_t pos_ = 0;
    std::string error_;

    void skipWs() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos_++;
            else break;
        }
    }

    bool fail(const char* msg) { if (error_.empty()) error_ = msg; return false; }

    bool parseValue(JsonValue& out) {
        skipWs();
        if (pos_ >= text_.size()) return fail("unexpected end of input");
        char c = text_[pos_];
        switch (c) {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': return parseString(out);
            case 't': case 'f': return parseBool(out);
            case 'n': return parseNull(out);
            default:  return parseNumber(out);
        }
    }

    bool parseObject(JsonValue& out) {
        out.type = JsonValue::Type::Object;
        pos_++; // consume '{'
        skipWs();
        if (pos_ < text_.size() && text_[pos_] == '}') { pos_++; return true; }
        while (true) {
            skipWs();
            if (pos_ >= text_.size() || text_[pos_] != '"')
                return fail("expected string key in object");
            JsonValue key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos_ >= text_.size() || text_[pos_] != ':')
                return fail("expected ':' after object key");
            pos_++; // consume ':'
            JsonValue val;
            if (!parseValue(val)) return false;
            out.obj.emplace_back(key.strVal, std::move(val));
            skipWs();
            if (pos_ >= text_.size()) return fail("unterminated object");
            if (text_[pos_] == ',') { pos_++; continue; }
            if (text_[pos_] == '}') { pos_++; return true; }
            return fail("expected ',' or '}' in object");
        }
    }

    bool parseArray(JsonValue& out) {
        out.type = JsonValue::Type::Array;
        pos_++; // consume '['
        skipWs();
        if (pos_ < text_.size() && text_[pos_] == ']') { pos_++; return true; }
        while (true) {
            JsonValue val;
            if (!parseValue(val)) return false;
            out.arr.push_back(std::move(val));
            skipWs();
            if (pos_ >= text_.size()) return fail("unterminated array");
            if (text_[pos_] == ',') { pos_++; continue; }
            if (text_[pos_] == ']') { pos_++; return true; }
            return fail("expected ',' or ']' in array");
        }
    }

    bool parseString(JsonValue& out) {
        out.type = JsonValue::Type::String;
        pos_++; // consume opening quote
        std::string s;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') { out.strVal = std::move(s); return true; }
            if (c == '\\') {
                if (pos_ >= text_.size()) return fail("unterminated escape");
                char e = text_[pos_++];
                switch (e) {
                    case '"':  s += '"';  break;
                    case '\\': s += '\\'; break;
                    case '/':  s += '/';  break;
                    case 'n':  s += '\n'; break;
                    case 't':  s += '\t'; break;
                    case 'r':  s += '\r'; break;
                    case 'b':  s += '\b'; break;
                    case 'f':  s += '\f'; break;
                    case 'u': {
                        // \uXXXX — BMP code point, encoded as UTF-8. No
                        // surrogate-pair handling (scene data is ASCII-centric;
                        // the serializer only emits \u00XX for control chars).
                        if (pos_ + 4 > text_.size()) return fail("unterminated \\u escape");
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = text_[pos_++];
                            cp <<= 4;
                            if      (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            else return fail("invalid \\u escape");
                        }
                        if (cp < 0x80) {
                            s += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            s += static_cast<char>(0xC0 | (cp >> 6));
                            s += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            s += static_cast<char>(0xE0 | (cp >> 12));
                            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            s += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default:   return fail("unsupported escape sequence");
                }
            } else {
                s += c;
            }
        }
        return fail("unterminated string");
    }

    bool parseBool(JsonValue& out) {
        if (text_.compare(pos_, 4, "true") == 0) {
            out.type = JsonValue::Type::Bool; out.boolVal = true; pos_ += 4; return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            out.type = JsonValue::Type::Bool; out.boolVal = false; pos_ += 5; return true;
        }
        return fail("invalid literal");
    }

    bool parseNull(JsonValue& out) {
        if (text_.compare(pos_, 4, "null") == 0) {
            out.type = JsonValue::Type::Null; pos_ += 4; return true;
        }
        return fail("invalid literal");
    }

    // Parse a JSON number (RFC 8259 grammar): an optional leading '-', an
    // integer part, an optional fraction, and an optional exponent. Validating
    // the shape here (rather than leaning on strtod) turns malformed tokens like
    // "1.2.3", "--5", or a lone "-" into clear parse failures.
    bool parseNumber(JsonValue& out) {
        size_t start = pos_;
        bool hasDigit = false;

        if (pos_ < text_.size() && text_[pos_] == '-') pos_++;   // JSON forbids leading '+'
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            pos_++; hasDigit = true;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            pos_++;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                pos_++; hasDigit = true;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            pos_++;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) pos_++;
            bool expDigit = false;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                pos_++; expDigit = true;
            }
            if (!expDigit) return fail("invalid number: exponent has no digits");
        }
        if (!hasDigit) return fail("invalid number");

        out.type = JsonValue::Type::Number;
        out.numVal = std::strtod(text_.c_str() + start, nullptr);
        return true;
    }
};

// --- Serializer ----------------------------------------------------------------

namespace detail {

// Append `s` as a quoted JSON string with proper escaping.
inline void appendEscaped(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

// Shortest decimal representation that round-trips back to the same double:
// try increasing %g precision until strtod recovers the value exactly.
inline void appendNumber(std::string& out, double v) {
    if (v != v || v > 1.7976931348623157e308 || v < -1.7976931348623157e308) {
        out += "0";   // JSON has no NaN/Infinity; scenes never contain them
        return;
    }
    char buf[32];
    // Integral values in the exactly-representable range print as plain
    // integers ("250", not "2.5e+02").
    if (v == static_cast<double>(static_cast<long long>(v))
        && v >= -9007199254740992.0 && v <= 9007199254740992.0) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out += buf;
        return;
    }
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    out += buf;
}

inline void serializeValue(const JsonValue& v, std::string& out,
                           int indent, int depth) {
    const std::string pad(static_cast<size_t>(indent) * (depth + 1), ' ');
    const std::string closePad(static_cast<size_t>(indent) * depth, ' ');
    switch (v.type) {
        case JsonValue::Type::Null:   out += "null"; break;
        case JsonValue::Type::Bool:   out += v.boolVal ? "true" : "false"; break;
        case JsonValue::Type::Number: appendNumber(out, v.numVal); break;
        case JsonValue::Type::String: appendEscaped(out, v.strVal); break;

        case JsonValue::Type::Array:
            if (v.arr.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t i = 0; i < v.arr.size(); ++i) {
                out += pad;
                serializeValue(v.arr[i], out, indent, depth + 1);
                if (i + 1 < v.arr.size()) out += ',';
                out += '\n';
            }
            out += closePad;
            out += ']';
            break;

        case JsonValue::Type::Object:
            if (v.obj.empty()) { out += "{}"; break; }
            out += "{\n";
            for (size_t i = 0; i < v.obj.size(); ++i) {
                out += pad;
                appendEscaped(out, v.obj[i].first);
                out += ": ";
                serializeValue(v.obj[i].second, out, indent, depth + 1);
                if (i + 1 < v.obj.size()) out += ',';
                out += '\n';
            }
            out += closePad;
            out += '}';
            break;
    }
}

} // namespace detail

// Pretty-print a JsonValue. Object keys keep their insertion order (see
// JsonValue::obj), so parse → serialize preserves the input's key order.
// Numbers use the shortest representation that parses back to the same double,
// so serialize → parse is lossless. Output has no trailing newline.
inline std::string serialize(const JsonValue& v, int indent = 2) {
    std::string out;
    detail::serializeValue(v, out, indent, 0);
    return out;
}

// Convenience: read a whole file into a string. Returns false if unreadable.
inline bool readFile(const char* path, std::string& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(n));
    size_t rd = std::fread(&out[0], 1, static_cast<size_t>(n), f);
    std::fclose(f);
    out.resize(rd);
    return true;
}

} // namespace cv
