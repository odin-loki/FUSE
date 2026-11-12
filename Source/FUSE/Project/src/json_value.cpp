#include <fuse/project/json_value.hpp>

#include <fuse/project/manifest.hpp>

#include <charconv>

namespace fuse::project::json {

Value::Value() = default;
Value::Value(const Value&) = default;
Value::Value(Value&&) noexcept = default;
Value& Value::operator=(const Value&) = default;
Value& Value::operator=(Value&&) noexcept = default;
Value::~Value() = default;

const Value* Value::get(std::string_view key) const {
    if (type != Type::Object) {
        return nullptr;
    }
    for (const auto& [name, value] : object) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

namespace {

constexpr u32 kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    std::expected<Value, ParseError> run() {
        Value root;
        skipSpace();
        if (!value(root, 0)) {
            return std::unexpected(m_error);
        }
        skipSpace();
        if (m_pos != m_text.size()) {
            fail("unexpected trailing characters");
            return std::unexpected(m_error);
        }
        return root;
    }

private:
    std::string_view m_text;
    usize m_pos = 0;
    ParseError m_error;

    bool fail(std::string_view message) {
        u32 line = 1;
        u32 column = 1;
        for (usize i = 0; i < m_pos && i < m_text.size(); ++i) {
            if (m_text[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        m_error = {line, column, std::string(message)};
        return false;
    }

    void skipSpace() {
        while (m_pos < m_text.size()) {
            const char ch = m_text[m_pos];
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                break;
            }
            ++m_pos;
        }
    }

    bool literal(std::string_view word) {
        if (m_text.substr(m_pos, word.size()) != word) {
            return fail("invalid literal");
        }
        m_pos += word.size();
        return true;
    }

    bool string(std::string& out) {
        ++m_pos; // opening quote
        const usize start = m_pos;
        while (m_pos < m_text.size() && m_text[m_pos] != '"') {
            const unsigned char ch = static_cast<unsigned char>(m_text[m_pos]);
            if (ch < 0x20u) {
                return fail("control character in string");
            }
            m_pos += m_text[m_pos] == '\\' ? 2u : 1u;
        }
        if (m_pos >= m_text.size()) {
            return fail("unterminated string");
        }
        out = unescapeJsonString(m_text.substr(start, m_pos - start));
        ++m_pos;
        return true;
    }

    bool number(Value& out) {
        const usize start = m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '-') {
            ++m_pos;
        }
        while (m_pos < m_text.size()) {
            const char ch = m_text[m_pos];
            if ((ch < '0' || ch > '9') && ch != '.' && ch != 'e' && ch != 'E' && ch != '+' && ch != '-') {
                break;
            }
            ++m_pos;
        }
        double parsed = 0.0;
        const char* first = m_text.data() + start;
        const char* last = m_text.data() + m_pos;
        const auto [end, ec] = std::from_chars(first, last, parsed);
        if (ec != std::errc{} || end != last || start == m_pos) {
            m_pos = start;
            return fail("invalid number");
        }
        out.type = Value::Type::Number;
        out.number = parsed;
        return true;
    }

    bool value(Value& out, u32 depth) {
        if (depth > kMaxDepth) {
            return fail("nesting too deep");
        }
        if (m_pos >= m_text.size()) {
            return fail("unexpected end of input");
        }
        const char ch = m_text[m_pos];
        if (ch == '{') {
            return objectValue(out, depth);
        }
        if (ch == '[') {
            return arrayValue(out, depth);
        }
        if (ch == '"') {
            out.type = Value::Type::String;
            return string(out.string);
        }
        if (ch == 't' || ch == 'f') {
            out.type = Value::Type::Bool;
            out.boolean = ch == 't';
            return literal(ch == 't' ? "true" : "false");
        }
        if (ch == 'n') {
            out.type = Value::Type::Null;
            return literal("null");
        }
        return number(out);
    }

    bool objectValue(Value& out, u32 depth) {
        out.type = Value::Type::Object;
        ++m_pos;
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        while (true) {
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != '"') {
                return fail("expected a member name");
            }
            std::string key;
            if (!string(key)) {
                return false;
            }
            if (out.get(key) != nullptr) {
                return fail("duplicate key \"" + key + "\"");
            }
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':') {
                return fail("expected ':'");
            }
            ++m_pos;
            skipSpace();
            Value member;
            if (!value(member, depth + 1u)) {
                return false;
            }
            out.object.emplace_back(std::move(key), std::move(member));
            skipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool arrayValue(Value& out, u32 depth) {
        out.type = Value::Type::Array;
        ++m_pos;
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        while (true) {
            skipSpace();
            Value item;
            if (!value(item, depth + 1u)) {
                return false;
            }
            out.array.push_back(std::move(item));
            skipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }
};

} // namespace

std::expected<Value, ParseError> parse(std::string_view text) {
    return Parser(text).run();
}

} // namespace fuse::project::json
