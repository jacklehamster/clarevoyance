// json.h — a minimal, dependency-free JSON parser for the game data layer.
//
// Scope: just enough JSON to load scene/event data files. Supports objects,
// arrays, strings, numbers, true/false/null. Not a general-purpose library —
// no streaming, no unicode escapes beyond the common ones, no comments.
//
// Header-only. Parsing is deterministic and allocation-simple; object members
// keep insertion order (a small vector, not a hash map) which keeps data-file
// round-trips predictable.
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
