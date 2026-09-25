// FUSE Relight RL-6.3: small JSON reader (see setup_json.hpp).
#include "setup_json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace fuse::relight::setup::json {

const Value* Value::get(std::string_view key) const {
    if (kind != Kind::Object) {
        return nullptr;
    }
    for (const auto& [k, v] : o) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

std::string Value::str(std::string_view key, std::string_view fallback) const {
    const Value* v = get(key);
    return v && v->kind == Kind::String ? v->s : std::string(fallback);
}

double Value::num(std::string_view key, double fallback) const {
    const Value* v = get(key);
    return v && v->kind == Kind::Number ? v->n : fallback;
}

bool Value::flag(std::string_view key, bool fallback) const {
    const Value* v = get(key);
    return v && v->kind == Kind::Bool ? v->b : fallback;
}

namespace {

constexpr int kMaxDepth = 256;

struct Parser {
    std::string_view t;
    std::size_t i = 0;
    std::string err;

    bool fail(const char* what) {
        if (err.empty()) {
            err = "offset " + std::to_string(i) + ": " + what;
        }
        return false;
    }
    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) {
            ++i;
        }
    }
    bool lit(std::string_view word) {
        if (t.substr(i, word.size()) != word) {
            return fail("invalid literal");
        }
        i += word.size();
        return true;
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
    bool hex4(std::uint32_t& out) {
        if (i + 4 > t.size()) {
            return fail("truncated \\u escape");
        }
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return fail("bad \\u escape");
            }
        }
        return true;
    }
    bool string(std::string& out) {
        ++i; // opening quote
        while (true) {
            if (i >= t.size()) {
                return fail("unterminated string");
            }
            const char c = t[i++];
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
            if (i >= t.size()) {
                return fail("unterminated escape");
            }
            const char e = t[i++];
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
                if (cp >= 0xD800 && cp < 0xDC00 && t.substr(i, 2) == "\\u") {
                    i += 2;
                    std::uint32_t lo = 0;
                    if (!hex4(lo)) {
                        return false;
                    }
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        return fail("bad surrogate pair");
                    }
                }
                utf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
    }
    bool numberValue(double& out) {
        const std::size_t start = i;
        if (i < t.size() && t[i] == '-') {
            ++i;
        }
        bool digits = false;
        while (i < t.size() && ((t[i] >= '0' && t[i] <= '9') || t[i] == '.' || t[i] == 'e' || t[i] == 'E' ||
                                t[i] == '+' || t[i] == '-')) {
            digits = digits || (t[i] >= '0' && t[i] <= '9');
            ++i;
        }
        if (!digits) {
            return fail("bad number");
        }
        const std::string text(t.substr(start, i - start));
        char* end = nullptr;
        out = std::strtod(text.c_str(), &end);
        if (!end || *end != '\0') {
            return fail("bad number");
        }
        return true;
    }
    bool value(Value& v, int depth) {
        if (depth > kMaxDepth) {
            return fail("nesting too deep");
        }
        ws();
        if (i >= t.size()) {
            return fail("unexpected end");
        }
        const char c = t[i];
        if (c == '{') {
            v.kind = Value::Kind::Object;
            ++i;
            ws();
            if (i < t.size() && t[i] == '}') {
                ++i;
                return true;
            }
            while (true) {
                ws();
                if (i >= t.size() || t[i] != '"') {
                    return fail("expected a member name");
                }
                std::string key;
                if (!string(key)) {
                    return false;
                }
                ws();
                if (i >= t.size() || t[i] != ':') {
                    return fail("expected ':'");
                }
                ++i;
                Value member;
                if (!value(member, depth + 1)) {
                    return false;
                }
                v.o.emplace_back(std::move(key), std::move(member));
                ws();
                if (i < t.size() && t[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < t.size() && t[i] == '}') {
                    ++i;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.kind = Value::Kind::Array;
            ++i;
            ws();
            if (i < t.size() && t[i] == ']') {
                ++i;
                return true;
            }
            while (true) {
                Value element;
                if (!value(element, depth + 1)) {
                    return false;
                }
                v.a.push_back(std::move(element));
                ws();
                if (i < t.size() && t[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < t.size() && t[i] == ']') {
                    ++i;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.kind = Value::Kind::String;
            return string(v.s);
        }
        if (c == 't') {
            v.kind = Value::Kind::Bool;
            v.b = true;
            return lit("true");
        }
        if (c == 'f') {
            v.kind = Value::Kind::Bool;
            v.b = false;
            return lit("false");
        }
        if (c == 'n') {
            v.kind = Value::Kind::Null;
            return lit("null");
        }
        v.kind = Value::Kind::Number;
        return numberValue(v.n);
    }
};

} // namespace

std::optional<Value> parse(std::string_view text, std::string* error) {
    Parser p;
    p.t = text;
    Value v;
    if (text.substr(0, 3) == "\xEF\xBB\xBF") {
        p.i = 3; // UTF-8 BOM (editors on Windows)
    }
    if (!p.value(v, 0)) {
        if (error) {
            *error = p.err;
        }
        return std::nullopt;
    }
    p.ws();
    if (p.i != text.size()) {
        p.fail("trailing characters");
        if (error) {
            *error = p.err;
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
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

std::string number(double v) {
    if (!std::isfinite(v)) {
        return "0";
    }
    if (std::fabs(v) < 9007199254740992.0 && v == std::floor(v)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", v);
        return buf;
    }
    char buf[40];
    for (int precision = 6; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (std::strtod(buf, nullptr) == v) {
            break;
        }
    }
    return buf;
}

} // namespace fuse::relight::setup::json
