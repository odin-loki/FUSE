// FUSE Relight RL-3.1: USDA value text <-> fuse::relight::mods::usd::Value.
#include <fuse/relight/mods/usd/usd_value.hpp>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace fuse::relight::mods::usd {

Value Value::makeBool(bool b) {
    Value v;
    v.kind = Kind::Bool;
    v.boolean = b;
    return v;
}

Value Value::makeNumber(double n) {
    Value v;
    v.kind = Kind::Number;
    v.number = n;
    v.text = formatNumber(n);
    return v;
}

Value Value::makeString(std::string s) {
    Value v;
    v.kind = Kind::String;
    v.text = std::move(s);
    return v;
}

Value Value::makeAsset(std::string authored) {
    Value v;
    v.kind = Kind::Asset;
    v.text = std::move(authored);
    return v;
}

Value Value::makePath(std::string path) {
    Value v;
    v.kind = Kind::Path;
    v.text = std::move(path);
    return v;
}

std::optional<double> Value::asNumber() const {
    if (kind == Kind::Number) {
        return number;
    }
    if (kind == Kind::Bool) {
        return boolean ? 1.0 : 0.0;
    }
    return std::nullopt;
}

std::optional<bool> Value::asBool() const {
    if (kind == Kind::Bool) {
        return boolean;
    }
    if (kind == Kind::Number) {
        return number != 0.0;
    }
    return std::nullopt;
}

std::optional<std::string> Value::asString() const {
    if (kind == Kind::String) {
        return text;
    }
    return std::nullopt;
}

std::optional<std::vector<double>> Value::asNumbers() const {
    if (kind != Kind::Tuple && kind != Kind::Array) {
        return std::nullopt;
    }
    std::vector<double> out;
    out.reserve(items.size());
    for (const Value& i : items) {
        const auto n = i.asNumber();
        if (!n) {
            return std::nullopt;
        }
        out.push_back(*n);
    }
    return out;
}

const Value& SharedValue::noneValue() noexcept {
    static const Value kNone;
    return kNone;
}

const Value* Value::get(std::string_view key) const {
    for (const auto& [k, v] : dict) {
        if (k == key) {
            return &v;
        }
        const std::size_t sp = k.find(' '); // "type name": type names have no spaces, entry names may
        if (sp != std::string::npos && std::string_view(k).substr(sp + 1) == key) {
            return &v;
        }
    }
    return nullptr;
}

// ---- parser -------------------------------------------------------------------------------------------------

namespace {

class Parser {
public:
    explicit Parser(std::string_view s) : m_s(s) {}

    std::optional<Value> parseTop(std::string* err) {
        Value v;
        if (!value(v)) {
            fail(err);
            return std::nullopt;
        }
        skipWs();
        if (m_i != m_s.size()) {
            m_err = "trailing text";
            fail(err);
            return std::nullopt;
        }
        return v;
    }

private:
    void fail(std::string* err) const {
        if (err) {
            *err = "offset " + std::to_string(m_i) + ": " + (m_err.empty() ? "syntax error" : m_err);
        }
    }
    void skipWs() {
        while (m_i < m_s.size()) {
            const char c = m_s[m_i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++m_i;
            } else if (c == '#') { // comment to end of line
                while (m_i < m_s.size() && m_s[m_i] != '\n') {
                    ++m_i;
                }
            } else {
                break;
            }
        }
    }
    bool peek(char c) {
        skipWs();
        return m_i < m_s.size() && m_s[m_i] == c;
    }
    bool eat(char c) {
        if (peek(c)) {
            ++m_i;
            return true;
        }
        return false;
    }
    bool startsWith(std::string_view w) const { return m_s.substr(m_i, w.size()) == w; }
    static bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == '.'; }

    bool word(std::string& out) {
        skipWs();
        const std::size_t b = m_i;
        while (m_i < m_s.size() && (identChar(m_s[m_i]) || m_s[m_i] == '[' || m_s[m_i] == ']')) {
            // "token[]" type names in dictionaries: only accept "[]" directly after identifier chars.
            if (m_s[m_i] == '[') {
                if (m_i + 1 < m_s.size() && m_s[m_i + 1] == ']') {
                    m_i += 2;
                    continue;
                }
                break;
            }
            if (m_s[m_i] == ']') {
                break;
            }
            ++m_i;
        }
        out.assign(m_s.substr(b, m_i - b));
        return !out.empty();
    }

    bool number(Value& v) {
        skipWs();
        const std::size_t b = m_i;
        if (m_i < m_s.size() && (m_s[m_i] == '-' || m_s[m_i] == '+')) {
            ++m_i;
        }
        if (startsWith("inf")) {
            m_i += 3;
            v.kind = Value::Kind::Number;
            v.text.assign(m_s.substr(b, m_i - b));
            v.number = m_s[b] == '-' ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
            return true;
        }
        if (startsWith("nan")) {
            m_i += 3;
            v.kind = Value::Kind::Number;
            v.text.assign(m_s.substr(b, m_i - b));
            v.number = std::numeric_limits<double>::quiet_NaN();
            return true;
        }
        bool digits = false;
        while (m_i < m_s.size()) {
            const char c = m_s[m_i];
            if (std::isdigit(static_cast<unsigned char>(c))) {
                digits = true;
                ++m_i;
            } else if (c == '.' || c == 'e' || c == 'E' ||
                       ((c == '-' || c == '+') && m_i > b && (m_s[m_i - 1] == 'e' || m_s[m_i - 1] == 'E'))) {
                ++m_i;
            } else {
                break;
            }
        }
        if (!digits) {
            m_i = b;
            m_err = "expected a value";
            return false;
        }
        v.kind = Value::Kind::Number;
        v.text.assign(m_s.substr(b, m_i - b));
        v.number = std::strtod(v.text.c_str(), nullptr);
        return true;
    }

    bool string(Value& v) {
        skipWs();
        const char q = m_s[m_i];
        const bool triple = startsWith(q == '"' ? "\"\"\"" : "'''");
        m_i += triple ? 3 : 1;
        std::string out;
        while (m_i < m_s.size()) {
            const char c = m_s[m_i];
            if (triple ? startsWith(q == '"' ? "\"\"\"" : "'''") : c == q) {
                m_i += triple ? 3 : 1;
                v.kind = Value::Kind::String;
                v.text = std::move(out);
                return true;
            }
            if (c == '\\' && m_i + 1 < m_s.size()) {
                const char e = m_s[m_i + 1];
                m_i += 2;
                switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '0': out += '\0'; break;
                default: out += e; break;
                }
                continue;
            }
            if (!triple && c == '\n') {
                break;
            }
            out += c;
            ++m_i;
        }
        m_err = "unterminated string";
        return false;
    }

    bool asset(Value& v) {
        skipWs();
        const bool triple = startsWith("@@@");
        m_i += triple ? 3 : 1;
        const std::string_view close = triple ? "@@@" : "@";
        const std::size_t e = m_s.find(close, m_i);
        if (e == std::string_view::npos) {
            m_err = "unterminated asset path";
            return false;
        }
        v.kind = Value::Kind::Asset;
        v.text.assign(m_s.substr(m_i, e - m_i));
        m_i = e + close.size();
        return true;
    }

    bool path(Value& v) {
        skipWs();
        const std::size_t e = m_s.find('>', m_i + 1);
        if (e == std::string_view::npos) {
            m_err = "unterminated path";
            return false;
        }
        v.kind = Value::Kind::Path;
        v.text.assign(m_s.substr(m_i + 1, e - m_i - 1));
        m_i = e + 1;
        return true;
    }

    bool list(Value& v, char open, char close, Value::Kind kind) {
        if (!eat(open)) {
            return false;
        }
        v.kind = kind;
        if (eat(close)) {
            return true;
        }
        while (true) {
            Value item;
            if (!value(item)) {
                return false;
            }
            v.items.push_back(std::move(item));
            if (eat(',')) {
                if (eat(close)) { // trailing comma
                    return true;
                }
                continue;
            }
            if (eat(close)) {
                return true;
            }
            m_err = std::string("expected ',' or '") + close + "'";
            return false;
        }
    }

    bool dict(Value& v) {
        if (!eat('{')) {
            return false;
        }
        v.kind = Value::Kind::Dict;
        while (true) {
            skipWs();
            if (eat('}')) {
                return true;
            }
            std::string key;
            // Time-sample dictionaries use number keys ("0: value", "-1.5: value").
            const char c = m_i < m_s.size() ? m_s[m_i] : '\0';
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.') {
                Value k;
                if (!number(k)) {
                    return false;
                }
                key = k.text;
                if (!eat(':')) {
                    m_err = "expected ':' after time code";
                    return false;
                }
            } else if (c == '"' || c == '\'') {
                Value k; // "key": value  (quoted keys)
                if (!string(k)) {
                    return false;
                }
                key = k.text;
                if (!eat(':') && !eat('=')) {
                    m_err = "expected ':' or '=' after key";
                    return false;
                }
            } else {
                std::string type, name;
                if (!word(type)) {
                    m_err = "expected a dictionary key";
                    return false;
                }
                skipWs();
                if (eat(':')) {
                    key = type;
                } else {
                    if (peek('"') || peek('\'')) {
                        Value q;
                        if (!string(q)) {
                            return false;
                        }
                        name = q.text;
                    } else if (!word(name)) {
                        m_err = "expected a dictionary entry name";
                        return false;
                    }
                    key = type + " " + name;
                    if (!eat('=')) {
                        m_err = "expected '=' in dictionary entry";
                        return false;
                    }
                }
            }
            Value item;
            if (!value(item)) {
                return false;
            }
            v.dict.emplace_back(std::move(key), std::move(item));
            eat(',');
            eat(';');
        }
    }

    bool value(Value& v) {
        skipWs();
        if (m_i >= m_s.size()) {
            m_err = "unexpected end of value";
            return false;
        }
        const char c = m_s[m_i];
        if (c == '"' || c == '\'') {
            return string(v);
        }
        if (c == '@') {
            return asset(v);
        }
        if (c == '<') {
            return path(v);
        }
        if (c == '(') {
            return list(v, '(', ')', Value::Kind::Tuple);
        }
        if (c == '[') {
            return list(v, '[', ']', Value::Kind::Array);
        }
        if (c == '{') {
            return dict(v);
        }
        if (startsWith("None")) {
            m_i += 4;
            v.kind = Value::Kind::None;
            return true;
        }
        if (startsWith("true")) {
            m_i += 4;
            v = Value::makeBool(true);
            return true;
        }
        if (startsWith("false")) {
            m_i += 5;
            v = Value::makeBool(false);
            return true;
        }
        return number(v);
    }

    std::string_view m_s;
    std::size_t m_i = 0;
    std::string m_err;
};

std::string escape(std::string_view s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default: out += c; break;
        }
    }
    return out;
}

} // namespace

std::optional<Value> parseValue(std::string_view text, std::string* err) {
    Parser p(text);
    return p.parseTop(err);
}

std::string formatNumber(double v) {
    if (std::isnan(v)) {
        return "nan";
    }
    if (std::isinf(v)) {
        return v > 0 ? "inf" : "-inf";
    }
    if (v == 0.0) {
        return "0";
    }
    char buf[40];
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        std::snprintf(buf, sizeof buf, "%.0f", v);
        return buf;
    }
    for (int p = 1; p <= 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, v);
        if (std::strtod(buf, nullptr) == v) {
            break;
        }
    }
    return buf;
}

std::string formatValue(const Value& v) {
    switch (v.kind) {
    case Value::Kind::None: return "None";
    case Value::Kind::Bool: return v.boolean ? "true" : "false";
    case Value::Kind::Number: return formatNumber(v.number);
    case Value::Kind::String: return "\"" + escape(v.text) + "\"";
    case Value::Kind::Asset: return "@" + v.text + "@";
    case Value::Kind::Path: return "<" + v.text + ">";
    case Value::Kind::Tuple:
    case Value::Kind::Array: {
        const bool tuple = v.kind == Value::Kind::Tuple;
        std::string s = tuple ? "(" : "[";
        for (std::size_t i = 0; i < v.items.size(); ++i) {
            s += (i ? ", " : "") + formatValue(v.items[i]);
        }
        return s + (tuple ? ")" : "]");
    }
    case Value::Kind::Dict: {
        std::string s = "{";
        for (std::size_t i = 0; i < v.dict.size(); ++i) {
            const auto& [k, item] = v.dict[i];
            s += (i ? "; " : " ");
            const bool timeKey = !k.empty() && (std::isdigit(static_cast<unsigned char>(k[0])) || k[0] == '-');
            s += k + (timeKey ? ": " : " = ") + formatValue(item);
        }
        return s + (v.dict.empty() ? "}" : " }");
    }
    }
    return "None";
}

std::string jsonQuote(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
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

namespace {

std::vector<std::string> segments(std::string_view p) {
    std::vector<std::string> out;
    std::size_t b = 0;
    while (b <= p.size()) {
        std::size_t e = p.find('/', b);
        if (e == std::string_view::npos) {
            e = p.size();
        }
        if (e > b) {
            out.emplace_back(p.substr(b, e - b));
        }
        b = e + 1;
    }
    return out;
}

/// `path` relative to directory `base` ("../" where needed) when both are absolute or both relative.
std::string relativeTo(std::string_view path, std::string_view base) {
    if (base.empty() || path.empty() || (path[0] == '/') != (base[0] == '/')) {
        return std::string(path);
    }
    const std::vector<std::string> p = segments(path), b = segments(base);
    std::size_t common = 0;
    while (common < p.size() && common < b.size() && p[common] == b[common]) {
        ++common;
    }
    std::string out;
    for (std::size_t i = common; i < b.size(); ++i) {
        out += "../";
    }
    for (std::size_t i = common; i < p.size(); ++i) {
        out += p[i] + (i + 1 < p.size() ? "/" : "");
    }
    return out;
}

} // namespace

std::string valueToJson(const Value& v, std::string_view resolveBase) {
    switch (v.kind) {
    case Value::Kind::None: return "null";
    case Value::Kind::Bool: return v.boolean ? "true" : "false";
    case Value::Kind::Number:
        if (std::isnan(v.number) || std::isinf(v.number)) {
            return jsonQuote(formatNumber(v.number));
        }
        return formatNumber(v.number);
    case Value::Kind::String: return jsonQuote(v.text);
    case Value::Kind::Asset:
        return "{\"asset\": " + jsonQuote(v.text) + ", \"resolved\": " + jsonQuote(relativeTo(v.resolved, resolveBase)) + "}";
    case Value::Kind::Path: return "{\"path\": " + jsonQuote(v.text) + "}";
    case Value::Kind::Tuple:
    case Value::Kind::Array: {
        std::string s = "[";
        for (std::size_t i = 0; i < v.items.size(); ++i) {
            s += (i ? ", " : "") + valueToJson(v.items[i], resolveBase);
        }
        return s + "]";
    }
    case Value::Kind::Dict: {
        std::string s = "{";
        for (std::size_t i = 0; i < v.dict.size(); ++i) {
            s += (i ? ", " : "") + jsonQuote(v.dict[i].first) + ": " + valueToJson(v.dict[i].second, resolveBase);
        }
        return s + "}";
    }
    }
    return "null";
}

std::string normalizePath(std::string_view path) {
    std::string p(path);
    for (char& c : p) {
        if (c == '\\') {
            c = '/';
        }
    }
    std::string prefix;
    std::size_t i = 0;
    if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') {
        prefix = p.substr(0, 2);
        i = 2;
    }
    const bool absolute = i < p.size() && p[i] == '/';
    if (absolute) {
        prefix += '/';
    }
    std::vector<std::string> parts;
    std::size_t b = i;
    while (b <= p.size()) {
        std::size_t e = p.find('/', b);
        if (e == std::string::npos) {
            e = p.size();
        }
        const std::string part = p.substr(b, e - b);
        if (part.empty() || part == ".") {
            // skip
        } else if (part == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute) {
                parts.push_back(part);
            }
        } else {
            parts.push_back(part);
        }
        b = e + 1;
    }
    std::string out = prefix;
    for (std::size_t k = 0; k < parts.size(); ++k) {
        out += (k ? "/" : "") + parts[k];
    }
    if (out.empty()) {
        out = ".";
    }
    return out;
}

std::string parentDir(std::string_view path) {
    const std::size_t s = path.rfind('/');
    if (s == std::string_view::npos) {
        return "";
    }
    if (s == 0) {
        return "/";
    }
    return std::string(path.substr(0, s));
}

std::string anchorAssetPath(std::string_view dir, std::string_view asset) {
    if (asset.empty()) {
        return "";
    }
    const bool absolute = asset[0] == '/' || asset[0] == '\\' ||
                          (asset.size() >= 2 && std::isalpha(static_cast<unsigned char>(asset[0])) && asset[1] == ':');
    if (absolute) {
        return normalizePath(asset);
    }
    // "scheme:..." (URLs, omniverse://) stay as authored.
    const std::size_t colon = asset.find(':');
    if (colon != std::string_view::npos && colon > 1 && asset.substr(0, colon).find('/') == std::string_view::npos) {
        return std::string(asset);
    }
    if (dir.empty()) {
        return normalizePath(asset);
    }
    return normalizePath(std::string(dir) + "/" + std::string(asset));
}

} // namespace fuse::relight::mods::usd
