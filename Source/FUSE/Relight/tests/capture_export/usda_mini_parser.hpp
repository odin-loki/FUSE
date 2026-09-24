// FUSE Relight RL-1.8 tests: a minimal USDA (text USD) parser.
//
// Plan RL-1.8: "USDA parses with TinyUSDZ (or our parser if W2.2 is late)". Remaster W2.2 (vendored
// TinyUSDZ / LightUSD) is not in the tree yet (Engine/lib/assimp/contrib/tinyusdz holds only Assimp's patch
// notes, not the library), so the capture tests parse with this one. It reads the USDA subset the Remix
// capture layout uses: the layer header and its metadata (dictionaries included), def / over / class prims
// with a type, a name and a metadata block (list-op keys such as "prepend references", @asset@</path>
// references), attributes ([custom] [uniform] type[[]] name [= value] [( metadata )], .timeSamples and
// .connect), relationships (rel name = <path> | [<path>, ...]), and values: numbers (nan / inf), strings,
// tokens, @asset@ paths, <paths>, tuples, arrays, dictionaries and None. Anything else is a parse error
// with the line number, which is what the tests want. Composition (references, sublayers) is not done
// here; capture_check.hpp resolves the references the capture layout uses by hand.
#pragma once

#include <cctype>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rl_usda {

struct Value {
    enum class Kind { None, Number, String, Token, Asset, Path, Tuple, Array, Dict };
    Kind kind = Kind::None;
    double n = 0.0;
    std::string s;      ///< String / Token / Asset text / Path text / Number literal
    std::string target; ///< Asset immediately followed by a <path> (a reference): the prim path
    std::vector<Value> items;                         ///< Tuple / Array
    std::vector<std::pair<std::string, Value>> dict;  ///< Dict: "type name" keys (layer / prim data) or time codes

    const Value* get(std::string_view key) const {
        for (const auto& kv : dict) {
            if (kv.first == key) {
                return &kv.second;
            }
            // Dictionary entries are keyed "type name"; match by name too.
            const std::size_t sp = kv.first.rfind(' ');
            if (sp != std::string::npos && std::string_view(kv.first).substr(sp + 1) == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }
};

struct Property {
    bool custom = false, uniform = false, relationship = false;
    std::string type; ///< "point3f[]", "matrix4d", ... ("rel" for relationships)
    std::string name;
    bool hasDefault = false;
    Value value;
    std::vector<std::pair<double, Value>> timeSamples;
    std::string connect; ///< .connect target
    std::vector<std::pair<std::string, Value>> metadata;
};

struct Prim {
    std::string specifier; ///< def / over / class
    std::string type;
    std::string name;
    std::vector<std::pair<std::string, Value>> metadata;
    std::vector<Property> properties;
    std::vector<Prim> children;

    const Prim* child(std::string_view n) const {
        for (const Prim& c : children) {
            if (c.name == n) {
                return &c;
            }
        }
        return nullptr;
    }
    const Property* property(std::string_view n) const {
        for (const Property& p : properties) {
            if (p.name == n) {
                return &p;
            }
        }
        return nullptr;
    }
    const Value* meta(std::string_view key) const {
        for (const auto& kv : metadata) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }
};

struct Layer {
    std::vector<std::pair<std::string, Value>> metadata;
    std::vector<Prim> roots;

    const Value* meta(std::string_view key) const {
        for (const auto& kv : metadata) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }
    /// "/A/B/C" (absolute prim path).
    const Prim* find(std::string_view path) const {
        if (path.empty() || path[0] != '/') {
            return nullptr;
        }
        const std::vector<Prim>* level = &roots;
        const Prim* found = nullptr;
        std::size_t at = 1;
        while (at <= path.size()) {
            const std::size_t next = path.find('/', at);
            const std::string_view name = path.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at);
            found = nullptr;
            for (const Prim& p : *level) {
                if (p.name == name) {
                    found = &p;
                    break;
                }
            }
            if (!found) {
                return nullptr;
            }
            level = &found->children;
            if (next == std::string_view::npos) {
                break;
            }
            at = next + 1;
        }
        return found;
    }
};

namespace detail {

struct Token {
    enum class Kind { End, Ident, Number, String, Asset, Path, Punct };
    Kind kind = Kind::End;
    std::string text;
    double n = 0.0;
    int line = 0;
};

class Lexer {
public:
    explicit Lexer(std::string_view t) : m_t(t) {}
    std::vector<Token> run(std::string& error) {
        std::vector<Token> out;
        while (true) {
            skip();
            Token tok;
            tok.line = m_line;
            if (m_i >= m_t.size()) {
                out.push_back(tok);
                return out;
            }
            const char c = m_t[m_i];
            if (c == '"') {
                tok.kind = Token::Kind::String;
                if (!string(tok.text)) {
                    error = "line " + std::to_string(m_line) + ": unterminated string";
                    return {};
                }
            } else if (c == '@') {
                tok.kind = Token::Kind::Asset;
                const std::size_t end = m_t.find('@', m_i + 1);
                if (end == std::string_view::npos) {
                    error = "line " + std::to_string(m_line) + ": unterminated asset path";
                    return {};
                }
                tok.text = std::string(m_t.substr(m_i + 1, end - m_i - 1));
                m_i = end + 1;
            } else if (c == '<') {
                tok.kind = Token::Kind::Path;
                const std::size_t end = m_t.find('>', m_i + 1);
                if (end == std::string_view::npos) {
                    error = "line " + std::to_string(m_line) + ": unterminated path";
                    return {};
                }
                tok.text = std::string(m_t.substr(m_i + 1, end - m_i - 1));
                m_i = end + 1;
            } else if (std::isdigit(static_cast<unsigned char>(c)) || ((c == '-' || c == '+' || c == '.') && m_i + 1 < m_t.size() &&
                                                                       (std::isdigit(static_cast<unsigned char>(m_t[m_i + 1])) || m_t[m_i + 1] == '.' ||
                                                                        m_t[m_i + 1] == 'i' || m_t[m_i + 1] == 'n'))) {
                tok.kind = Token::Kind::Number;
                const std::string rest(m_t.substr(m_i, 64));
                char* end = nullptr;
                tok.n = std::strtod(rest.c_str(), &end);
                if (end == rest.c_str()) {
                    error = "line " + std::to_string(m_line) + ": bad number";
                    return {};
                }
                tok.text = rest.substr(0, static_cast<std::size_t>(end - rest.c_str()));
                m_i += tok.text.size();
            } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                tok.kind = Token::Kind::Ident;
                std::size_t j = m_i;
                while (j < m_t.size() && (std::isalnum(static_cast<unsigned char>(m_t[j])) || m_t[j] == '_' || m_t[j] == ':' ||
                                          m_t[j] == '.' || m_t[j] == '[' || m_t[j] == ']')) {
                    // "type[]" is one identifier; a lone '[' after a space is punctuation.
                    if ((m_t[j] == '[' || m_t[j] == ']') && !(m_t[j] == '[' && j + 1 < m_t.size() && m_t[j + 1] == ']') &&
                        !(m_t[j] == ']' && j > 0 && m_t[j - 1] == '[')) {
                        break;
                    }
                    ++j;
                }
                // A trailing ':' belongs to a dictionary key, not the identifier.
                while (j > m_i + 1 && m_t[j - 1] == ':') {
                    --j;
                }
                tok.text = std::string(m_t.substr(m_i, j - m_i));
                m_i = j;
            } else if (std::string_view("()[]{}=,:").find(c) != std::string_view::npos) {
                tok.kind = Token::Kind::Punct;
                tok.text = std::string(1, c);
                ++m_i;
            } else {
                error = "line " + std::to_string(m_line) + ": unexpected character '" + std::string(1, c) + "'";
                return {};
            }
            out.push_back(std::move(tok));
        }
    }

private:
    void skip() {
        while (m_i < m_t.size()) {
            const char c = m_t[m_i];
            if (c == '\n') {
                ++m_line;
                ++m_i;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++m_i;
            } else if (c == '#') {
                while (m_i < m_t.size() && m_t[m_i] != '\n') {
                    ++m_i;
                }
            } else {
                return;
            }
        }
    }
    bool string(std::string& out) {
        ++m_i;
        while (m_i < m_t.size() && m_t[m_i] != '"') {
            char c = m_t[m_i++];
            if (c == '\\' && m_i < m_t.size()) {
                const char e = m_t[m_i++];
                c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
            } else if (c == '\n') {
                ++m_line;
            }
            out += c;
        }
        if (m_i >= m_t.size()) {
            return false;
        }
        ++m_i;
        return true;
    }
    std::string_view m_t;
    std::size_t m_i = 0;
    int m_line = 1;
};

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : m_t(std::move(toks)) {}

    bool layer(Layer& out, std::string& error) {
        if (!metadataBlock(out.metadata, true)) {
            error = m_error;
            return false;
        }
        while (ok() && peek().kind != Token::Kind::End) {
            Prim p;
            if (!prim(p)) {
                break;
            }
            out.roots.push_back(std::move(p));
        }
        error = m_error;
        return m_error.empty();
    }

private:
    const Token& peek(std::size_t k = 0) const { return m_t[std::min(m_i + k, m_t.size() - 1)]; }
    Token next() { return m_t[std::min(m_i++, m_t.size() - 1)]; }
    bool ok() const { return m_error.empty(); }
    bool fail(const std::string& why) {
        if (m_error.empty()) {
            m_error = "line " + std::to_string(peek().line) + ": " + why + " (at '" + peek().text + "')";
        }
        return false;
    }
    bool isPunct(const char* p, std::size_t k = 0) const { return peek(k).kind == Token::Kind::Punct && peek(k).text == p; }
    bool expect(const char* p) {
        if (!isPunct(p)) {
            return fail(std::string("expected '") + p + "'");
        }
        ++m_i;
        return true;
    }

    /// `( key = value ... )`; optional unless `required`.
    bool metadataBlock(std::vector<std::pair<std::string, Value>>& out, bool required) {
        if (!isPunct("(")) {
            return !required || fail("expected a metadata block");
        }
        ++m_i;
        while (ok() && !isPunct(")")) {
            if (peek().kind == Token::Kind::String) { // a layer doc string
                out.emplace_back("doc", Value{Value::Kind::String, 0.0, next().text, {}, {}, {}});
                continue;
            }
            if (peek().kind != Token::Kind::Ident) {
                return fail("expected a metadata key");
            }
            std::string key = next().text;
            if ((key == "prepend" || key == "append" || key == "add" || key == "delete" || key == "reorder") &&
                peek().kind == Token::Kind::Ident) {
                key += " " + next().text;
            }
            if (!expect("=")) {
                return false;
            }
            Value v;
            if (!value(v)) {
                return false;
            }
            out.emplace_back(std::move(key), std::move(v));
        }
        return expect(")");
    }

    /// `{ type name = value ... }` (layer / prim dictionaries) or `{ time: value, ... }` (time samples).
    bool dictionary(Value& out) {
        out.kind = Value::Kind::Dict;
        if (!expect("{")) {
            return false;
        }
        while (ok() && !isPunct("}")) {
            if (peek().kind == Token::Kind::Number) {
                const std::string key = next().text;
                if (!expect(":")) {
                    return false;
                }
                Value v;
                if (!value(v)) {
                    return false;
                }
                out.dict.emplace_back(key, std::move(v));
                if (isPunct(",")) {
                    ++m_i;
                }
                continue;
            }
            if (peek().kind != Token::Kind::Ident || peek(1).kind != Token::Kind::Ident) {
                return fail("expected 'type name' in a dictionary");
            }
            const std::string type = next().text;
            const std::string name = next().text;
            if (!expect("=")) {
                return false;
            }
            Value v;
            if (!value(v)) {
                return false;
            }
            out.dict.emplace_back(type + " " + name, std::move(v));
        }
        return expect("}");
    }

    bool value(Value& out) {
        const Token& t = peek();
        switch (t.kind) {
        case Token::Kind::Number:
            out.kind = Value::Kind::Number;
            out.s = t.text; // the literal (64-bit integers do not fit a double)
            out.n = next().n;
            return true;
        case Token::Kind::String:
            out.kind = Value::Kind::String;
            out.s = next().text;
            return true;
        case Token::Kind::Asset:
            out.kind = Value::Kind::Asset;
            out.s = next().text;
            if (peek().kind == Token::Kind::Path) {
                out.target = next().text;
            }
            return true;
        case Token::Kind::Path:
            out.kind = Value::Kind::Path;
            out.s = next().text;
            return true;
        case Token::Kind::Ident:
            if (t.text == "None") {
                ++m_i;
                out.kind = Value::Kind::None;
                return true;
            }
            if (t.text == "nan" || t.text == "inf") {
                out.kind = Value::Kind::Number;
                out.n = std::strtod(next().text.c_str(), nullptr);
                return true;
            }
            out.kind = Value::Kind::Token;
            out.s = next().text;
            return true;
        case Token::Kind::Punct:
            if (t.text == "(" || t.text == "[") {
                const bool tuple = t.text == "(";
                out.kind = tuple ? Value::Kind::Tuple : Value::Kind::Array;
                ++m_i;
                const char* close = tuple ? ")" : "]";
                while (ok() && !isPunct(close)) {
                    Value e;
                    if (!value(e)) {
                        return false;
                    }
                    out.items.push_back(std::move(e));
                    if (isPunct(",")) {
                        ++m_i;
                    } else if (!isPunct(close)) {
                        return fail(std::string("expected ',' or '") + close + "'");
                    }
                }
                return expect(close);
            }
            if (t.text == "{") {
                return dictionary(out);
            }
            return fail("unexpected punctuation in a value");
        case Token::Kind::End: return fail("unexpected end of file in a value");
        }
        return fail("bad value");
    }

    bool prim(Prim& p) {
        if (peek().kind != Token::Kind::Ident || (peek().text != "def" && peek().text != "over" && peek().text != "class")) {
            return fail("expected def / over / class");
        }
        p.specifier = next().text;
        if (peek().kind == Token::Kind::Ident) {
            p.type = next().text;
        }
        if (peek().kind != Token::Kind::String) {
            return fail("expected a prim name");
        }
        p.name = next().text;
        if (!metadataBlock(p.metadata, false) || !expect("{")) {
            return false;
        }
        while (ok() && !isPunct("}")) {
            const Token& t = peek();
            if (t.kind != Token::Kind::Ident) {
                return fail("expected a property or a prim");
            }
            if (t.text == "def" || t.text == "over" || t.text == "class") {
                Prim c;
                if (!prim(c)) {
                    return false;
                }
                p.children.push_back(std::move(c));
                continue;
            }
            Property prop;
            if (!property(prop)) {
                return false;
            }
            // Merge ".timeSamples" / ".connect" into the property of the same name.
            Property* existing = nullptr;
            for (Property& q : p.properties) {
                if (q.name == prop.name) {
                    existing = &q;
                }
            }
            if (existing) {
                if (prop.hasDefault) {
                    existing->hasDefault = true;
                    existing->value = prop.value;
                }
                if (!prop.timeSamples.empty()) {
                    existing->timeSamples = prop.timeSamples;
                }
                if (!prop.connect.empty()) {
                    existing->connect = prop.connect;
                }
            } else {
                p.properties.push_back(std::move(prop));
            }
        }
        return expect("}");
    }

    bool property(Property& prop) {
        if (peek().text == "rel") {
            ++m_i;
            prop.relationship = true;
            prop.type = "rel";
        } else {
            while (peek().kind == Token::Kind::Ident && (peek().text == "custom" || peek().text == "uniform" || peek().text == "varying")) {
                const std::string q = next().text;
                prop.custom = prop.custom || q == "custom";
                prop.uniform = prop.uniform || q == "uniform";
            }
            if (peek().kind != Token::Kind::Ident) {
                return fail("expected a property type");
            }
            prop.type = next().text;
        }
        if (peek().kind != Token::Kind::Ident) {
            return fail("expected a property name");
        }
        std::string name = next().text;
        enum { Default, Samples, Connect } form = Default;
        if (name.size() > 12 && name.ends_with(".timeSamples")) {
            name.resize(name.size() - 12);
            form = Samples;
        } else if (name.size() > 8 && name.ends_with(".connect")) {
            name.resize(name.size() - 8);
            form = Connect;
        }
        prop.name = name;
        if (isPunct("=")) {
            ++m_i;
            Value v;
            if (!value(v)) {
                return false;
            }
            if (form == Samples) {
                if (v.kind != Value::Kind::Dict) {
                    return fail("timeSamples need a dictionary");
                }
                for (auto& kv : v.dict) {
                    prop.timeSamples.emplace_back(std::strtod(kv.first.c_str(), nullptr), std::move(kv.second));
                }
            } else if (form == Connect) {
                prop.connect = v.s;
            } else {
                prop.hasDefault = true;
                prop.value = std::move(v);
            }
        }
        return metadataBlock(prop.metadata, false);
    }

    std::vector<Token> m_t;
    std::size_t m_i = 0;
    std::string m_error;
};

} // namespace detail

/// Parses a USDA layer. The first line must be "#usda 1.0".
inline std::optional<Layer> parse(std::string_view text, std::string* error = nullptr) {
    auto fail = [&](const std::string& e) -> std::optional<Layer> {
        if (error) {
            *error = e;
        }
        return std::nullopt;
    };
    if (text.rfind("#usda 1.0", 0) != 0) {
        return fail("line 1: not a USDA 1.0 layer");
    }
    std::string err;
    std::vector<detail::Token> toks = detail::Lexer(text).run(err);
    if (!err.empty()) {
        return fail(err);
    }
    Layer layer;
    detail::Parser p(std::move(toks));
    if (!p.layer(layer, err)) {
        return fail(err);
    }
    return layer;
}

} // namespace rl_usda
