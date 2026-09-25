// FUSE Relight RL-6.3: a small JSON value and reader for the profile files and the capture records.
//
// Private to the setup package. The profile library is linked into d3d9.dll / d3d8.dll through the tap, so
// it cannot use the RL-1.8 reader (fuse_relight_capture_export links the tap itself). Objects keep their
// insertion order; numbers are doubles (hashes are always hex strings in our files).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fuse::relight::setup::json {

struct Value {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Value> a;
    std::vector<std::pair<std::string, Value>> o;

    bool isObject() const { return kind == Kind::Object; }
    bool isArray() const { return kind == Kind::Array; }
    bool isString() const { return kind == Kind::String; }
    bool isNumber() const { return kind == Kind::Number; }
    bool isBool() const { return kind == Kind::Bool; }

    /// Object member (nullptr when absent or not an object).
    const Value* get(std::string_view key) const;
    std::string str(std::string_view key, std::string_view fallback = {}) const;
    double num(std::string_view key, double fallback = 0.0) const;
    bool flag(std::string_view key, bool fallback = false) const;
};

/// One JSON document (RFC 8259; \u escapes of the BMP and surrogate pairs become UTF-8). nullopt on a
/// syntax error; `error` gets the byte offset and the reason.
std::optional<Value> parse(std::string_view text, std::string* error = nullptr);

/// JSON string literal of `s` (quotes and escapes).
std::string quote(std::string_view s);

/// Shortest round-tripping text of a number (integers print without a fraction).
std::string number(double v);

} // namespace fuse::relight::setup::json
