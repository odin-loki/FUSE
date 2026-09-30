#pragma once

// Small JSON DOM for engine settings files (action maps, config snippets). RFC 8259 input:
// objects keep member order, numbers are f64, strings are UTF-8 with \uXXXX escapes decoded
// (surrogate pairs included). No external dependencies.

#include <fuse/types.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fuse::config::json {

enum class Type : u8 { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;
    static Value boolean(bool b);
    static Value number(f64 n);
    static Value string(std::string s);
    static Value array();
    static Value object();

    [[nodiscard]] Type type() const { return m_type; }
    [[nodiscard]] bool is_null() const { return m_type == Type::Null; }
    [[nodiscard]] bool is_bool() const { return m_type == Type::Bool; }
    [[nodiscard]] bool is_number() const { return m_type == Type::Number; }
    [[nodiscard]] bool is_string() const { return m_type == Type::String; }
    [[nodiscard]] bool is_array() const { return m_type == Type::Array; }
    [[nodiscard]] bool is_object() const { return m_type == Type::Object; }

    [[nodiscard]] bool as_bool(bool fallback = false) const { return is_bool() ? m_bool : fallback; }
    [[nodiscard]] f64 as_number(f64 fallback = 0.0) const { return is_number() ? m_number : fallback; }
    [[nodiscard]] const std::string& as_string() const { return m_string; }

    [[nodiscard]] const std::vector<Value>& items() const { return m_items; }
    [[nodiscard]] const std::vector<std::pair<std::string, Value>>& members() const { return m_members; }
    /// Object member by key (first match); nullptr when absent or not an object.
    [[nodiscard]] const Value* find(std::string_view key) const;

    /// Array append / object insert (replaces an existing key). No-ops on other types.
    Value& push(Value v);
    Value& set(std::string_view key, Value v);

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    f64 m_number = 0.0;
    std::string m_string;
    std::vector<Value> m_items;
    std::vector<std::pair<std::string, Value>> m_members;
};

/// Parses a complete document. On failure returns false and sets `error` to "line:col: message".
bool parse(std::string_view text, Value& out, std::string* error = nullptr);

/// Serialises with two-space indentation (pretty) or compactly.
[[nodiscard]] std::string write(const Value& value, bool pretty = true);

} // namespace fuse::config::json
