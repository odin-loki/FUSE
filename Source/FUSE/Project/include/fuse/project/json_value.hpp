#pragma once

#include <fuse/types.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fuse::project::json {

/// Small strict JSON document (RFC 8259) for `project.json` schema 2. Object members keep file order; duplicate keys
/// are an error. No exceptions.
struct Value {
    enum class Type : u8 { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    [[nodiscard]] bool isBool() const { return type == Type::Bool; }
    [[nodiscard]] bool isNumber() const { return type == Type::Number; }
    [[nodiscard]] bool isString() const { return type == Type::String; }
    [[nodiscard]] bool isArray() const { return type == Type::Array; }
    [[nodiscard]] bool isObject() const { return type == Type::Object; }
    /// Object member or null.
    [[nodiscard]] const Value* get(std::string_view key) const;
};

struct ParseError {
    u32 line = 1;
    u32 column = 1;
    std::string message;
};

std::expected<Value, ParseError> parse(std::string_view text);

} // namespace fuse::project::json
