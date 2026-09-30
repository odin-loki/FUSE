#include <fuse/config/json.hpp>

#include <charconv>
#include <cmath>
#include <system_error>

namespace fuse::config::json {

Value Value::boolean(bool b) {
    Value v;
    v.m_type = Type::Bool;
    v.m_bool = b;
    return v;
}

Value Value::number(f64 n) {
    Value v;
    v.m_type = Type::Number;
    v.m_number = n;
    return v;
}

Value Value::string(std::string s) {
    Value v;
    v.m_type = Type::String;
    v.m_string = std::move(s);
    return v;
}

Value Value::array() {
    Value v;
    v.m_type = Type::Array;
    return v;
}

Value Value::object() {
    Value v;
    v.m_type = Type::Object;
    return v;
}

const Value* Value::find(std::string_view key) const {
    if (m_type != Type::Object) {
        return nullptr;
    }
    for (const auto& [k, v] : m_members) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

Value& Value::push(Value v) {
    if (m_type == Type::Array) {
        m_items.push_back(std::move(v));
        return m_items.back();
    }
    return *this;
}

Value& Value::set(std::string_view key, Value v) {
    if (m_type != Type::Object) {
        return *this;
    }
    for (auto& [k, existing] : m_members) {
        if (k == key) {
            existing = std::move(v);
            return existing;
        }
    }
    m_members.emplace_back(std::string(key), std::move(v));
    return m_members.back().second;
}

namespace {

constexpr u32 kMaxDepth = 256;

class Parser {
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    bool parse_document(Value& out, std::string* error) {
        skip_ws();
        if (!parse_value(out, 0)) {
            report(error);
            return false;
        }
        skip_ws();
        if (m_pos != m_text.size()) {
            fail("trailing characters after the document");
            report(error);
            return false;
        }
        return true;
    }

private:
    void report(std::string* error) const {
        if (error == nullptr) {
            return;
        }
        u32 line = 1;
        u32 col = 1;
        for (usize i = 0; i < m_errorPos && i < m_text.size(); ++i) {
            if (m_text[i] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        *error = std::to_string(line) + ":" + std::to_string(col) + ": " + m_message;
    }

    bool fail(const char* message) {
        if (m_message.empty()) {
            m_message = message;
            m_errorPos = m_pos;
        }
        return false;
    }

    void skip_ws() {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++m_pos;
            } else {
                break;
            }
        }
    }

    bool literal(std::string_view word) {
        if (m_text.substr(m_pos, word.size()) != word) {
            return fail("invalid literal");
        }
        m_pos += word.size();
        return true;
    }

    bool parse_value(Value& out, u32 depth) {
        if (depth > kMaxDepth) {
            return fail("nesting too deep");
        }
        if (m_pos >= m_text.size()) {
            return fail("unexpected end of input");
        }
        const char c = m_text[m_pos];
        switch (c) {
        case '{':
            return parse_object(out, depth);
        case '[':
            return parse_array(out, depth);
        case '"': {
            std::string s;
            if (!parse_string(s)) {
                return false;
            }
            out = Value::string(std::move(s));
            return true;
        }
        case 't':
            if (!literal("true")) {
                return false;
            }
            out = Value::boolean(true);
            return true;
        case 'f':
            if (!literal("false")) {
                return false;
            }
            out = Value::boolean(false);
            return true;
        case 'n':
            if (!literal("null")) {
                return false;
            }
            out = Value();
            return true;
        default:
            return parse_number(out);
        }
    }

    bool parse_number(Value& out) {
        const usize start = m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '-') {
            ++m_pos;
        }
        if (m_pos >= m_text.size() || m_text[m_pos] < '0' || m_text[m_pos] > '9') {
            m_pos = start;
            return fail("unexpected character");
        }
        if (m_text[m_pos] == '0' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] >= '0' && m_text[m_pos + 1] <= '9') {
            return fail("leading zero in number");
        }
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++m_pos;
            } else {
                break;
            }
        }
        f64 value = 0.0;
        const char* first = m_text.data() + start;
        const char* last = m_text.data() + m_pos;
        const auto res = std::from_chars(first, last, value);
        if (res.ec != std::errc() || res.ptr != last || !std::isfinite(value)) {
            m_pos = start;
            return fail("invalid number");
        }
        out = Value::number(value);
        return true;
    }

    static void append_utf8(std::string& s, u32 cp) {
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

    bool parse_hex4(u32& out) {
        if (m_pos + 4 > m_text.size()) {
            return fail("truncated \\u escape");
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<u32>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<u32>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<u32>(c - 'A' + 10);
            } else {
                return fail("invalid \\u escape");
            }
        }
        return true;
    }

    bool parse_string(std::string& out) {
        ++m_pos; // opening quote
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                --m_pos;
                return fail("control character in string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_pos >= m_text.size()) {
                break;
            }
            const char e = m_text[m_pos++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
                out += e;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                u32 cp = 0;
                if (!parse_hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    u32 low = 0;
                    if (m_text.substr(m_pos, 2) != "\\u") {
                        return fail("unpaired surrogate");
                    }
                    m_pos += 2;
                    if (!parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        return fail("unpaired surrogate");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail("unpaired surrogate");
                }
                append_utf8(out, cp);
                break;
            }
            default:
                --m_pos;
                return fail("invalid escape");
            }
        }
        return fail("unterminated string");
    }

    bool parse_array(Value& out, u32 depth) {
        ++m_pos;
        out = Value::array();
        skip_ws();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        for (;;) {
            skip_ws();
            Value item;
            if (!parse_value(item, depth + 1)) {
                return false;
            }
            out.push(std::move(item));
            skip_ws();
            if (m_pos >= m_text.size()) {
                return fail("unterminated array");
            }
            const char c = m_text[m_pos++];
            if (c == ']') {
                return true;
            }
            if (c != ',') {
                --m_pos;
                return fail("expected ',' or ']'");
            }
        }
    }

    bool parse_object(Value& out, u32 depth) {
        ++m_pos;
        out = Value::object();
        skip_ws();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        for (;;) {
            skip_ws();
            if (m_pos >= m_text.size() || m_text[m_pos] != '"') {
                return fail("expected a string key");
            }
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_ws();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':') {
                return fail("expected ':'");
            }
            ++m_pos;
            skip_ws();
            Value item;
            if (!parse_value(item, depth + 1)) {
                return false;
            }
            out.set(key, std::move(item));
            skip_ws();
            if (m_pos >= m_text.size()) {
                return fail("unterminated object");
            }
            const char c = m_text[m_pos++];
            if (c == '}') {
                return true;
            }
            if (c != ',') {
                --m_pos;
                return fail("expected ',' or '}'");
            }
        }
    }

    std::string_view m_text;
    usize m_pos = 0;
    usize m_errorPos = 0;
    std::string m_message;
};

void write_string(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char* kHex = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                out += kHex[static_cast<unsigned char>(c) & 0xF];
            } else {
                out += c;
            }
            break;
        }
    }
    out += '"';
}

void indent(std::string& out, bool pretty, u32 depth) {
    if (!pretty) {
        return;
    }
    out += '\n';
    out.append(static_cast<usize>(depth) * 2u, ' ');
}

void write_value(std::string& out, const Value& v, bool pretty, u32 depth) {
    switch (v.type()) {
    case Type::Null:
        out += "null";
        break;
    case Type::Bool:
        out += v.as_bool() ? "true" : "false";
        break;
    case Type::Number: {
        char buf[64];
        const auto res = std::to_chars(buf, buf + sizeof(buf), v.as_number());
        out.append(buf, res.ptr);
        break;
    }
    case Type::String:
        write_string(out, v.as_string());
        break;
    case Type::Array: {
        out += '[';
        if (v.items().empty()) {
            out += ']';
            break;
        }
        bool first = true;
        for (const Value& item : v.items()) {
            if (!first) {
                out += ',';
            }
            first = false;
            indent(out, pretty, depth + 1);
            write_value(out, item, pretty, depth + 1);
        }
        indent(out, pretty, depth);
        out += ']';
        break;
    }
    case Type::Object: {
        out += '{';
        if (v.members().empty()) {
            out += '}';
            break;
        }
        bool first = true;
        for (const auto& [key, item] : v.members()) {
            if (!first) {
                out += ',';
            }
            first = false;
            indent(out, pretty, depth + 1);
            write_string(out, key);
            out += pretty ? ": " : ":";
            write_value(out, item, pretty, depth + 1);
        }
        indent(out, pretty, depth);
        out += '}';
        break;
    }
    }
}

} // namespace

bool parse(std::string_view text, Value& out, std::string* error) {
    Parser parser(text);
    Value result;
    if (!parser.parse_document(result, error)) {
        return false;
    }
    out = std::move(result);
    return true;
}

std::string write(const Value& value, bool pretty) {
    std::string out;
    write_value(out, value, pretty, 0);
    if (pretty) {
        out += '\n';
    }
    return out;
}

} // namespace fuse::config::json
