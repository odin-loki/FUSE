// FUSE Remaster W0.1: a small header-only JSON value, reader and deterministic writer (plan §2.1-§2.3).
//
// Used by fuse_poco (POCO records), fuse_remaster_db (remaster.lock.json) and the fuse_remaster CLI. Objects keep
// their members sorted by key (std::map), so the writer's output is canonical: the same value always serialises
// to the same bytes. Numbers keep the exact integer when the lexeme has no fraction or exponent (u64 blob sizes
// survive), and doubles are written with the shortest round-trip form (std::to_chars). The reader is strict
// RFC 8259 (no comments, no trailing commas), bounded in nesting depth, and never throws: errors come back as a
// message with a byte offset.
#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fuse::remaster::json {

class Value {
public:
    enum class Type : std::uint8_t { Null, Bool, Int, Double, String, Array, Object };
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : m_type(Type::Bool), m_bool(b) {}
    Value(int i) : m_type(Type::Int), m_int(i) {}
    Value(std::int64_t i) : m_type(Type::Int), m_int(i) {}
    Value(std::uint32_t i) : m_type(Type::Int), m_int(i) {}
    Value(std::uint64_t u) {
        if (u <= static_cast<std::uint64_t>(INT64_MAX)) {
            m_type = Type::Int;
            m_int = static_cast<std::int64_t>(u);
        } else {
            m_type = Type::Double;
            m_double = static_cast<double>(u);
        }
    }
    Value(double d) : m_type(Type::Double), m_double(d) {}
    Value(float f) : m_type(Type::Double), m_double(static_cast<double>(f)) {}
    Value(const char* s) : m_type(Type::String), m_string(s) {}
    Value(std::string s) : m_type(Type::String), m_string(std::move(s)) {}
    Value(std::string_view s) : m_type(Type::String), m_string(s) {}
    Value(Array a) : m_type(Type::Array), m_array(std::move(a)) {}
    Value(Object o) : m_type(Type::Object), m_object(std::move(o)) {}

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isInt() const { return m_type == Type::Int; }
    bool isNumber() const { return m_type == Type::Int || m_type == Type::Double; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool asBool(bool fallback = false) const { return m_type == Type::Bool ? m_bool : fallback; }
    std::int64_t asInt(std::int64_t fallback = 0) const {
        if (m_type == Type::Int) {
            return m_int;
        }
        if (m_type == Type::Double && std::isfinite(m_double) && m_double >= -9.2e18 && m_double <= 9.2e18) {
            return static_cast<std::int64_t>(m_double);
        }
        return fallback;
    }
    double asDouble(double fallback = 0.0) const {
        return m_type == Type::Double ? m_double : m_type == Type::Int ? static_cast<double>(m_int) : fallback;
    }
    const std::string& asString() const {
        static const std::string kEmpty;
        return m_type == Type::String ? m_string : kEmpty;
    }
    const Array& asArray() const {
        static const Array kEmpty;
        return m_type == Type::Array ? m_array : kEmpty;
    }
    const Object& asObject() const {
        static const Object kEmpty;
        return m_type == Type::Object ? m_object : kEmpty;
    }
    Array& arr() {
        if (m_type != Type::Array) {
            *this = array();
        }
        return m_array;
    }
    Object& obj() {
        if (m_type != Type::Object) {
            *this = object();
        }
        return m_object;
    }

    /// Object member (created on demand; turns a non-object into an empty object first).
    Value& operator[](std::string_view key) {
        Object& o = obj();
        auto it = o.find(key);
        if (it == o.end()) {
            it = o.emplace(std::string(key), Value()).first;
        }
        return it->second;
    }
    /// Member lookup; nullptr when absent or not an object.
    const Value* get(std::string_view key) const {
        if (m_type != Type::Object) {
            return nullptr;
        }
        const auto it = m_object.find(key);
        return it == m_object.end() ? nullptr : &it->second;
    }
    void push(Value v) { arr().push_back(std::move(v)); }
    std::size_t size() const {
        return m_type == Type::Array ? m_array.size() : m_type == Type::Object ? m_object.size() : 0;
    }

    friend bool operator==(const Value& a, const Value& b) {
        if (a.isNumber() && b.isNumber()) {
            if (a.m_type == Type::Int && b.m_type == Type::Int) {
                return a.m_int == b.m_int;
            }
            return a.asDouble() == b.asDouble();
        }
        if (a.m_type != b.m_type) {
            return false;
        }
        switch (a.m_type) {
        case Type::Null: return true;
        case Type::Bool: return a.m_bool == b.m_bool;
        case Type::String: return a.m_string == b.m_string;
        case Type::Array: return a.m_array == b.m_array;
        case Type::Object: return a.m_object == b.m_object;
        default: return false;
        }
    }

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    std::int64_t m_int = 0;
    double m_double = 0.0;
    std::string m_string;
    Array m_array;
    Object m_object;
};

// ---- writer -------------------------------------------------------------------------------------------------

namespace detail {

inline void writeString(std::string& out, std::string_view s) {
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                static const char* kHex = "0123456789abcdef";
                out += "\\u00";
                out += kHex[c >> 4];
                out += kHex[c & 15];
            } else {
                out += ch;
            }
        }
    }
    out += '"';
}

inline void writeDouble(std::string& out, double d) {
    if (!std::isfinite(d)) {
        out += "null"; // JSON has no NaN / Inf
        return;
    }
    if (d == 0.0) {
        out += "0"; // -0 and 0 serialise the same (canonical)
        return;
    }
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), d);
    std::string_view s(buf, static_cast<std::size_t>(r.ptr - buf));
    out += s;
    // Keep doubles recognisable as non-integers only when they are; 2.0 prints "2" which reads back as Int 2:
    // equality treats Int 2 and Double 2.0 as equal, so round trips stay stable.
}

inline void write(std::string& out, const Value& v, int indent, int depth) {
    auto newline = [&](int d) {
        if (indent >= 0) {
            out += '\n';
            out.append(static_cast<std::size_t>(indent * d), ' ');
        }
    };
    switch (v.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
    case Value::Type::Int: out += std::to_string(v.asInt()); break;
    case Value::Type::Double: writeDouble(out, v.asDouble()); break;
    case Value::Type::String: writeString(out, v.asString()); break;
    case Value::Type::Array: {
        const auto& a = v.asArray();
        if (a.empty()) {
            out += "[]";
            break;
        }
        out += '[';
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (i) {
                out += ',';
            }
            newline(depth + 1);
            write(out, a[i], indent, depth + 1);
        }
        newline(depth);
        out += ']';
        break;
    }
    case Value::Type::Object: {
        const auto& o = v.asObject();
        if (o.empty()) {
            out += "{}";
            break;
        }
        out += '{';
        bool first = true;
        for (const auto& [k, m] : o) {
            if (!first) {
                out += ',';
            }
            first = false;
            newline(depth + 1);
            writeString(out, k);
            out += indent >= 0 ? ": " : ":";
            write(out, m, indent, depth + 1);
        }
        newline(depth);
        out += '}';
        break;
    }
    }
}

} // namespace detail

/// Compact canonical form (sorted keys, no whitespace).
inline std::string writeCompact(const Value& v) {
    std::string out;
    detail::write(out, v, -1, 0);
    return out;
}
/// Two-space pretty form with a trailing newline (sorted keys; the on-disk form of records and locks).
inline std::string writePretty(const Value& v) {
    std::string out;
    detail::write(out, v, 2, 0);
    out += '\n';
    return out;
}

// ---- reader -------------------------------------------------------------------------------------------------

struct ParseError {
    std::string message;
    std::size_t offset = 0;
};

namespace detail {

class Reader {
public:
    explicit Reader(std::string_view text) : m_s(text) {}

    std::optional<Value> run(ParseError* err) {
        Value v;
        skipWs();
        if (!value(v, 0)) {
            if (err) {
                *err = {m_error, m_pos};
            }
            return std::nullopt;
        }
        skipWs();
        if (m_pos != m_s.size()) {
            fail("trailing characters after the JSON value");
            if (err) {
                *err = {m_error, m_pos};
            }
            return std::nullopt;
        }
        return v;
    }

private:
    static constexpr int kMaxDepth = 256;

    bool fail(const char* msg) {
        if (m_error.empty()) {
            m_error = msg;
        }
        return false;
    }
    void skipWs() {
        while (m_pos < m_s.size() && (m_s[m_pos] == ' ' || m_s[m_pos] == '\t' || m_s[m_pos] == '\n' || m_s[m_pos] == '\r')) {
            ++m_pos;
        }
    }
    bool literal(std::string_view word) {
        if (m_s.substr(m_pos, word.size()) != word) {
            return fail("invalid literal");
        }
        m_pos += word.size();
        return true;
    }
    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) {
            return fail("nesting too deep");
        }
        if (m_pos >= m_s.size()) {
            return fail("unexpected end of input");
        }
        switch (m_s[m_pos]) {
        case 'n': out = Value(); return literal("null");
        case 't': out = Value(true); return literal("true");
        case 'f': out = Value(false); return literal("false");
        case '"': {
            std::string s;
            if (!string(s)) {
                return false;
            }
            out = Value(std::move(s));
            return true;
        }
        case '[': {
            ++m_pos;
            Value::Array a;
            skipWs();
            if (m_pos < m_s.size() && m_s[m_pos] == ']') {
                ++m_pos;
                out = Value(std::move(a));
                return true;
            }
            for (;;) {
                skipWs();
                Value e;
                if (!value(e, depth + 1)) {
                    return false;
                }
                a.push_back(std::move(e));
                skipWs();
                if (m_pos >= m_s.size()) {
                    return fail("unterminated array");
                }
                if (m_s[m_pos] == ',') {
                    ++m_pos;
                    continue;
                }
                if (m_s[m_pos] == ']') {
                    ++m_pos;
                    break;
                }
                return fail("expected ',' or ']'");
            }
            out = Value(std::move(a));
            return true;
        }
        case '{': {
            ++m_pos;
            Value::Object o;
            skipWs();
            if (m_pos < m_s.size() && m_s[m_pos] == '}') {
                ++m_pos;
                out = Value(std::move(o));
                return true;
            }
            for (;;) {
                skipWs();
                if (m_pos >= m_s.size() || m_s[m_pos] != '"') {
                    return fail("expected a member name");
                }
                std::string key;
                if (!string(key)) {
                    return false;
                }
                skipWs();
                if (m_pos >= m_s.size() || m_s[m_pos] != ':') {
                    return fail("expected ':'");
                }
                ++m_pos;
                skipWs();
                Value m;
                if (!value(m, depth + 1)) {
                    return false;
                }
                if (!o.emplace(std::move(key), std::move(m)).second) {
                    return fail("duplicate member name");
                }
                skipWs();
                if (m_pos >= m_s.size()) {
                    return fail("unterminated object");
                }
                if (m_s[m_pos] == ',') {
                    ++m_pos;
                    continue;
                }
                if (m_s[m_pos] == '}') {
                    ++m_pos;
                    break;
                }
                return fail("expected ',' or '}'");
            }
            out = Value(std::move(o));
            return true;
        }
        default: return number(out);
        }
    }
    static void appendUtf8(std::string& s, std::uint32_t cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(std::uint32_t& cp) {
        if (m_pos + 4 > m_s.size()) {
            return fail("truncated \\u escape");
        }
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_s[m_pos++];
            cp <<= 4;
            if (c >= '0' && c <= '9') {
                cp |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                cp |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                cp |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return fail("bad hex digit in \\u escape");
            }
        }
        return true;
    }
    bool string(std::string& out) {
        ++m_pos; // opening quote
        while (m_pos < m_s.size()) {
            const char c = m_s[m_pos++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("control character in string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_pos >= m_s.size()) {
                break;
            }
            const char e = m_s[m_pos++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    std::uint32_t lo = 0;
                    if (m_s.substr(m_pos, 2) != "\\u") {
                        return fail("unpaired high surrogate");
                    }
                    m_pos += 2;
                    if (!hex4(lo)) {
                        return false;
                    }
                    if (lo < 0xDC00 || lo > 0xDFFF) {
                        return fail("bad low surrogate");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail("unpaired low surrogate");
                }
                appendUtf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(Value& out) {
        const std::size_t start = m_pos;
        bool isInt = true;
        if (m_pos < m_s.size() && m_s[m_pos] == '-') {
            ++m_pos;
        }
        if (m_pos >= m_s.size() || m_s[m_pos] < '0' || m_s[m_pos] > '9') {
            return fail("unexpected character");
        }
        if (m_s[m_pos] == '0') {
            ++m_pos;
        } else {
            while (m_pos < m_s.size() && m_s[m_pos] >= '0' && m_s[m_pos] <= '9') {
                ++m_pos;
            }
        }
        if (m_pos < m_s.size() && m_s[m_pos] == '.') {
            isInt = false;
            ++m_pos;
            if (m_pos >= m_s.size() || m_s[m_pos] < '0' || m_s[m_pos] > '9') {
                return fail("digit expected after '.'");
            }
            while (m_pos < m_s.size() && m_s[m_pos] >= '0' && m_s[m_pos] <= '9') {
                ++m_pos;
            }
        }
        if (m_pos < m_s.size() && (m_s[m_pos] == 'e' || m_s[m_pos] == 'E')) {
            isInt = false;
            ++m_pos;
            if (m_pos < m_s.size() && (m_s[m_pos] == '+' || m_s[m_pos] == '-')) {
                ++m_pos;
            }
            if (m_pos >= m_s.size() || m_s[m_pos] < '0' || m_s[m_pos] > '9') {
                return fail("digit expected in exponent");
            }
            while (m_pos < m_s.size() && m_s[m_pos] >= '0' && m_s[m_pos] <= '9') {
                ++m_pos;
            }
        }
        const char* b = m_s.data() + start;
        const char* e = m_s.data() + m_pos;
        if (isInt) {
            std::int64_t i = 0;
            const auto r = std::from_chars(b, e, i);
            if (r.ec == std::errc() && r.ptr == e) {
                out = Value(i);
                return true;
            }
        }
        double d = 0.0;
        const auto r = std::from_chars(b, e, d);
        if (r.ptr != e || (r.ec != std::errc() && r.ec != std::errc::result_out_of_range)) {
            return fail("bad number");
        }
        if (!std::isfinite(d)) {
            return fail("number out of range");
        }
        out = Value(d);
        return true;
    }

    std::string_view m_s;
    std::size_t m_pos = 0;
    std::string m_error;
};

} // namespace detail

inline std::optional<Value> parse(std::string_view text, ParseError* error = nullptr) {
    return detail::Reader(text).run(error);
}

} // namespace fuse::remaster::json
