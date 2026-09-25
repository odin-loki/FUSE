// fuse_json_mini — a small, dependency-free JSON reader shared by the asset tooling
// (fuse_lint asset-licences, fuse_assetcheck). Parses RFC 8259 JSON into a tree of JsonValue.
// Not a general-purpose library: no streaming, numbers are doubles, \u escapes outside the BMP
// pass through surrogate pairs as UTF-8. Errors come back as a message with a byte offset.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::tools {

struct JsonMember;

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<JsonMember> obj; // insertion order kept (vector of an incomplete type is fine since C++17)

    [[nodiscard]] bool isNull() const { return kind == Kind::Null; }
    [[nodiscard]] bool isString() const { return kind == Kind::String; }
    [[nodiscard]] bool isNumber() const { return kind == Kind::Number; }
    [[nodiscard]] bool isBool() const { return kind == Kind::Bool; }
    [[nodiscard]] bool isArray() const { return kind == Kind::Array; }
    [[nodiscard]] bool isObject() const { return kind == Kind::Object; }

    /// Member lookup; returns a shared null value when absent or when this is not an object.
    [[nodiscard]] const JsonValue& operator[](std::string_view key) const;
    [[nodiscard]] bool has(std::string_view key) const;
    [[nodiscard]] std::string asString(std::string fallback = {}) const { return kind == Kind::String ? str : fallback; }
    [[nodiscard]] double asNumber(double fallback = 0.0) const { return kind == Kind::Number ? num : fallback; }
    [[nodiscard]] bool asBool(bool fallback = false) const { return kind == Kind::Bool ? b : fallback; }
};

struct JsonMember {
    std::string first;
    JsonValue second;
};

// Defined out of line: JsonValue must be complete before its member containers are instantiated (clang).
inline const JsonValue& JsonValue::operator[](std::string_view key) const {
    static const JsonValue kNull;
    if (kind == Kind::Object) {
        for (const auto& kv : obj) {
            if (kv.first == key) {
                return kv.second;
            }
        }
    }
    return kNull;
}

inline bool JsonValue::has(std::string_view key) const {
    if (kind != Kind::Object) {
        return false;
    }
    for (const auto& kv : obj) {
        if (kv.first == key) {
            return true;
        }
    }
    return false;
}

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : s_(text) {}

    /// Parses the whole document. On failure returns false and fills `error`.
    bool parse(JsonValue& out, std::string& error) {
        skipWs();
        if (!value(out, 0)) {
            error = err_ + " at byte " + std::to_string(pos_);
            return false;
        }
        skipWs();
        if (pos_ != s_.size()) {
            error = "trailing characters at byte " + std::to_string(pos_);
            return false;
        }
        return true;
    }

private:
    std::string_view s_;
    size_t pos_ = 0;
    std::string err_;

    bool fail(const char* what) {
        err_ = what;
        return false;
    }
    void skipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }
    bool literal(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) == lit) {
            pos_ += lit.size();
            return true;
        }
        return false;
    }
    static void putUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80u) {
            out.push_back(char(cp));
        } else if (cp < 0x800u) {
            out.push_back(char(0xC0u | (cp >> 6)));
            out.push_back(char(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            out.push_back(char(0xE0u | (cp >> 12)));
            out.push_back(char(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(char(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(char(0xF0u | (cp >> 18)));
            out.push_back(char(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(char(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(char(0x80u | (cp & 0x3Fu)));
        }
    }
    bool hex4(std::uint32_t& cp) {
        if (pos_ + 4 > s_.size()) {
            return fail("truncated \\u escape");
        }
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= std::uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= std::uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= std::uint32_t(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& out) {
        ++pos_; // opening quote
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20u) {
                return fail("control character in string");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= s_.size()) {
                break;
            }
            const char e = s_[pos_++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800u && cp < 0xDC00u && s_.substr(pos_, 2) == "\\u") {
                    pos_ += 2;
                    std::uint32_t lo = 0;
                    if (!hex4(lo)) {
                        return false;
                    }
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                }
                putUtf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(double& out) {
        const size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' || s_[pos_] == 'e' ||
                                    s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-')) {
            ++pos_;
        }
        const std::string tok(s_.substr(start, pos_ - start));
        char* end = nullptr;
        out = std::strtod(tok.c_str(), &end);
        if (tok.empty() || end != tok.c_str() + tok.size()) {
            return fail("bad number");
        }
        return true;
    }
    bool value(JsonValue& out, int depth) {
        if (depth > 128) {
            return fail("nesting too deep");
        }
        skipWs();
        if (pos_ >= s_.size()) {
            return fail("unexpected end");
        }
        const char c = s_[pos_];
        if (c == '{') {
            out.kind = JsonValue::Kind::Object;
            ++pos_;
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            while (true) {
                skipWs();
                if (pos_ >= s_.size() || s_[pos_] != '"') {
                    return fail("expected member name");
                }
                std::string key;
                if (!string(key)) {
                    return false;
                }
                skipWs();
                if (pos_ >= s_.size() || s_[pos_] != ':') {
                    return fail("expected ':'");
                }
                ++pos_;
                JsonValue v;
                if (!value(v, depth + 1)) {
                    return false;
                }
                out.obj.push_back(JsonMember{std::move(key), std::move(v)});
                skipWs();
                if (pos_ < s_.size() && s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < s_.size() && s_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            out.kind = JsonValue::Kind::Array;
            ++pos_;
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            while (true) {
                JsonValue v;
                if (!value(v, depth + 1)) {
                    return false;
                }
                out.arr.push_back(std::move(v));
                skipWs();
                if (pos_ < s_.size() && s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < s_.size() && s_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            out.kind = JsonValue::Kind::String;
            return string(out.str);
        }
        if (literal("true")) {
            out.kind = JsonValue::Kind::Bool;
            out.b = true;
            return true;
        }
        if (literal("false")) {
            out.kind = JsonValue::Kind::Bool;
            out.b = false;
            return true;
        }
        if (literal("null")) {
            out.kind = JsonValue::Kind::Null;
            return true;
        }
        out.kind = JsonValue::Kind::Number;
        return number(out.num);
    }
};

inline bool parseJson(std::string_view text, JsonValue& out, std::string& error) {
    JsonParser p(text);
    return p.parse(out, error);
}

} // namespace fuse::tools
