// FUSE Relight RL-1.8: a small JSON value, reader and writer for the capture writers.
//
// The POCO store shim (poco_store.hpp) writes and re-reads its records with it, and the tap replay tools
// read the recording tap's JSON Lines and the RL-0.4 app sidecars. Objects keep their insertion order
// (the writers sort keys themselves where determinism needs it). Numbers are doubles; 64-bit hashes are
// always written as hex strings, never as numbers.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fuse::relight::capture::exporter::json {

struct Value {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Value> a;
    std::vector<std::pair<std::string, Value>> o;

    static Value null() { return {}; }
    static Value boolean(bool v);
    static Value number(double v);
    static Value string(std::string v);
    static Value array();
    static Value object();

    bool isNull() const { return kind == Kind::Null; }
    bool isObject() const { return kind == Kind::Object; }
    bool isArray() const { return kind == Kind::Array; }
    bool isString() const { return kind == Kind::String; }
    bool isNumber() const { return kind == Kind::Number; }

    /// Object member (nullptr when absent or not an object).
    const Value* get(std::string_view key) const;
    /// Object member access for building: inserts a null member when absent.
    Value& operator[](std::string_view key);
    /// Array append for building.
    Value& push(Value v);

    std::string str(std::string_view key, std::string_view fallback = {}) const;
    double num(std::string_view key, double fallback = 0.0) const;
    std::uint32_t u32(std::string_view key, std::uint32_t fallback = 0) const;
    bool flag(std::string_view key, bool fallback = false) const;
};

/// Parses one JSON document (RFC 8259; \u escapes of the BMP become UTF-8). nullopt on a syntax error
/// (`error` gets the byte offset and reason).
std::optional<Value> parse(std::string_view text, std::string* error = nullptr);

/// Compact serialisation (no spaces), members in insertion order. Numbers use the shortest %.17g form
/// that round-trips; integers up to 2^53 print without a fraction.
std::string write(const Value& v);
/// Two-space indented serialisation with a trailing newline (the POCO files: small and diffable).
std::string writePretty(const Value& v);

/// JSON string literal of `s` (quotes and escapes).
std::string quote(std::string_view s);

} // namespace fuse::relight::capture::exporter::json
