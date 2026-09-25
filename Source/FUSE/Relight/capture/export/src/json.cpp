// FUSE Relight RL-1.8: JSON value, reader and writer (see json.hpp).
#include <fuse/relight/capture/export/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fuse::relight::capture::exporter::json {

Value Value::boolean(bool v) {
    Value r;
    r.kind = Kind::Bool;
    r.b = v;
    return r;
}
Value Value::number(double v) {
    Value r;
    r.kind = Kind::Number;
    r.n = v;
    return r;
}
Value Value::string(std::string v) {
    Value r;
    r.kind = Kind::String;
    r.s = std::move(v);
    return r;
}
Value Value::array() {
    Value r;
    r.kind = Kind::Array;
    return r;
}
Value Value::object() {
    Value r;
    r.kind = Kind::Object;
    return r;
}

const Value* Value::get(std::string_view key) const {
    if (kind != Kind::Object) {
        return nullptr;
    }
    for (const auto& kv : o) {
        if (kv.first == key) {
            return &kv.second;
        }
    }
    return nullptr;
}

Value& Value::operator[](std::string_view key) {
    if (kind != Kind::Object) {
        *this = object();
    }
    for (auto& kv : o) {
        if (kv.first == key) {
            return kv.second;
        }
    }
    o.emplace_back(std::string(key), Value());
    return o.back().second;
}

Value& Value::push(Value v) {
    if (kind != Kind::Array) {
        *this = array();
    }
    a.push_back(std::move(v));
    return a.back();
}

std::string Value::str(std::string_view key, std::string_view fallback) const {
    const Value* v = get(key);
    return v && v->kind == Kind::String ? v->s : std::string(fallback);
}

double Value::num(std::string_view key, double fallback) const {
    const Value* v = get(key);
    return v && v->kind == Kind::Number ? v->n : fallback;
}

std::uint32_t Value::u32(std::string_view key, std::uint32_t fallback) const {
    const Value* v = get(key);
    if (!v || v->kind != Kind::Number || !(v->n >= 0.0) || v->n > 4294967295.0) {
        return fallback;
    }
    return static_cast<std::uint32_t>(v->n);
}

bool Value::flag(std::string_view key, bool fallback) const {
    const Value* v = get(key);
    return v && v->kind == Kind::Bool ? v->b : fallback;
}

// ---- reader ---------------------------------------------------------------------------------------------

namespace {

class Reader {
public:
    explicit Reader(std::string_view t) : m_t(t) {}

    bool document(Value& out) {
        value(out, 0);
        ws();
        if (m_ok && m_i != m_t.size()) {
            fail("trailing characters");
        }
        return m_ok;
    }
    std::string error() const { return "offset " + std::to_string(m_errorAt) + ": " + m_error; }

private:
    void fail(const char* why) {
        if (m_ok) {
            m_ok = false;
            m_error = why;
            m_errorAt = m_i;
        }
    }
    void ws() {
        while (m_i < m_t.size() && (m_t[m_i] == ' ' || m_t[m_i] == '\t' || m_t[m_i] == '\r' || m_t[m_i] == '\n')) {
            ++m_i;
        }
    }
    bool eat(char c) {
        ws();
        if (m_i < m_t.size() && m_t[m_i] == c) {
            ++m_i;
            return true;
        }
        return false;
    }
    static void utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(std::uint32_t& cp) {
        if (m_i + 4 > m_t.size()) {
            return false;
        }
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = m_t[m_i++];
            cp <<= 4;
            if (c >= '0' && c <= '9') {
                cp |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                cp |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                cp |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }
    void string(std::string& out) {
        if (!eat('"')) {
            return fail("expected a string");
        }
        while (m_i < m_t.size() && m_t[m_i] != '"') {
            const char c = m_t[m_i++];
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("control character in a string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_i >= m_t.size()) {
                return fail("unterminated escape");
            }
            const char e = m_t[m_i++];
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
                    return fail("bad \\u escape");
                }
                if (cp >= 0xD800 && cp < 0xDC00 && m_i + 6 <= m_t.size() && m_t[m_i] == '\\' && m_t[m_i + 1] == 'u') {
                    m_i += 2;
                    std::uint32_t lo = 0;
                    if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) {
                        return fail("bad surrogate pair");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: return fail("unknown escape");
            }
        }
        if (m_i >= m_t.size()) {
            return fail("unterminated string");
        }
        ++m_i;
    }
    void value(Value& v, int depth) {
        if (depth > 512) {
            return fail("nesting too deep");
        }
        ws();
        if (m_i >= m_t.size()) {
            return fail("unexpected end");
        }
        const char c = m_t[m_i];
        if (c == '{') {
            ++m_i;
            v = Value::object();
            if (eat('}')) {
                return;
            }
            do {
                std::pair<std::string, Value> kv;
                ws();
                string(kv.first);
                if (!m_ok) {
                    return;
                }
                if (!eat(':')) {
                    return fail("expected ':'");
                }
                value(kv.second, depth + 1);
                v.o.push_back(std::move(kv));
            } while (m_ok && eat(','));
            if (m_ok && !eat('}')) {
                fail("expected '}'");
            }
        } else if (c == '[') {
            ++m_i;
            v = Value::array();
            if (eat(']')) {
                return;
            }
            do {
                v.a.emplace_back();
                value(v.a.back(), depth + 1);
            } while (m_ok && eat(','));
            if (m_ok && !eat(']')) {
                fail("expected ']'");
            }
        } else if (c == '"') {
            v = Value::string({});
            string(v.s);
        } else if (m_t.compare(m_i, 4, "true") == 0) {
            v = Value::boolean(true);
            m_i += 4;
        } else if (m_t.compare(m_i, 5, "false") == 0) {
            v = Value::boolean(false);
            m_i += 5;
        } else if (m_t.compare(m_i, 4, "null") == 0) {
            v = Value();
            m_i += 4;
        } else {
            // strtod needs a terminated buffer: copy the number's characters.
            std::size_t j = m_i;
            while (j < m_t.size() && (std::strchr("+-0123456789.eE", m_t[j]) != nullptr)) {
                ++j;
            }
            const std::string num(m_t.substr(m_i, j - m_i));
            char* end = nullptr;
            const double d = std::strtod(num.c_str(), &end);
            if (num.empty() || end != num.c_str() + num.size()) {
                return fail("bad number");
            }
            v = Value::number(d);
            m_i = j;
        }
    }

    std::string_view m_t;
    std::size_t m_i = 0;
    bool m_ok = true;
    std::string m_error;
    std::size_t m_errorAt = 0;
};

void numberText(std::string& out, double d) {
    if (!std::isfinite(d)) {
        out += "null";
        return;
    }
    char buf[40];
    if (d == std::floor(d) && std::fabs(d) < 9007199254740992.0) {
        std::snprintf(buf, sizeof buf, "%.0f", d);
        if (std::strcmp(buf, "-0") == 0) {
            std::snprintf(buf, sizeof buf, "0");
        }
    } else {
        for (int p = 9; p <= 17; ++p) {
            std::snprintf(buf, sizeof buf, "%.*g", p, d);
            if (std::strtod(buf, nullptr) == d) {
                break;
            }
        }
    }
    out += buf;
}

void writeValue(std::string& out, const Value& v, int indent, int level) {
    const bool pretty = indent > 0;
    auto newline = [&](int l) {
        if (pretty) {
            out += '\n';
            out.append(static_cast<std::size_t>(indent * l), ' ');
        }
    };
    switch (v.kind) {
    case Value::Kind::Null: out += "null"; break;
    case Value::Kind::Bool: out += v.b ? "true" : "false"; break;
    case Value::Kind::Number: numberText(out, v.n); break;
    case Value::Kind::String: out += quote(v.s); break;
    case Value::Kind::Array: {
        out += '[';
        // Arrays of scalars stay on one line (streams of numbers, key lists).
        bool scalars = true;
        for (const Value& e : v.a) {
            scalars = scalars && e.kind != Value::Kind::Array && e.kind != Value::Kind::Object;
        }
        for (std::size_t k = 0; k < v.a.size(); ++k) {
            if (k) {
                out += pretty && scalars ? ", " : ",";
            }
            if (!scalars) {
                newline(level + 1);
            }
            writeValue(out, v.a[k], indent, level + 1);
        }
        if (!scalars && !v.a.empty()) {
            newline(level);
        }
        out += ']';
        break;
    }
    case Value::Kind::Object: {
        out += '{';
        for (std::size_t k = 0; k < v.o.size(); ++k) {
            if (k) {
                out += ',';
            }
            newline(level + 1);
            out += quote(v.o[k].first);
            out += pretty ? ": " : ":";
            writeValue(out, v.o[k].second, indent, level + 1);
        }
        if (!v.o.empty()) {
            newline(level);
        }
        out += '}';
        break;
    }
    }
}

} // namespace

std::optional<Value> parse(std::string_view text, std::string* error) {
    Value v;
    Reader r(text);
    if (!r.document(v)) {
        if (error) {
            *error = r.error();
        }
        return std::nullopt;
    }
    return v;
}

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

std::string write(const Value& v) {
    std::string out;
    writeValue(out, v, 0, 0);
    return out;
}

std::string writePretty(const Value& v) {
    std::string out;
    writeValue(out, v, 2, 0);
    out += '\n';
    return out;
}

} // namespace fuse::relight::capture::exporter::json
