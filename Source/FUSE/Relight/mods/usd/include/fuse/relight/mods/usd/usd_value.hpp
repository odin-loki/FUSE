// FUSE Relight RL-3.1: USD values as FUSE sees them (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.3).
//
// The layer loader never exposes TinyUSDZ types: every authored value (attribute defaults, time samples,
// metadata, dictionaries) reaches FUSE as the USDA text TinyUSDZ prints for it, parsed here into a small
// tree. That keeps the composition engine, the Remix view and the importer (RL-3.2) independent of the
// vendored library's value representation, and it is the same syntax RL-1.8's capture writer emits.
//
// Grammar (USDA value subset): numbers (incl. nan / inf / -inf), true / false, "strings" / 'strings' /
// """long strings""" (escapes \" \\ \n \t \'), @asset@ / @@@asset@@@, <paths>, (tuples), [arrays],
// { dictionaries } whose entries are `type name = value` (customData) or `key: value` (time samples),
// None (a value block).
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fuse::relight::mods::usd {

struct Value {
    enum class Kind : unsigned char { None, Bool, Number, String, Asset, Path, Tuple, Array, Dict };
    Kind kind = Kind::None;
    bool boolean = false;
    double number = 0.0;
    /// String: the unescaped text. Asset: the authored path. Path: the path text. Number: the literal.
    std::string text;
    /// Asset only: the path anchored to the directory of the layer that authored it (plan §4.3), '/'
    /// separated and normalized; empty when the authored path is empty.
    std::string resolved;
    std::vector<Value> items; ///< Tuple / Array
    /// Dict: (key, value). customData keys are "type name" as authored; time-sample keys are the time text.
    std::vector<std::pair<std::string, Value>> dict;

    static Value none() { return {}; }
    static Value makeBool(bool b);
    static Value makeNumber(double n);
    static Value makeString(std::string s);
    static Value makeAsset(std::string authored);
    static Value makePath(std::string path);

    bool isNone() const { return kind == Kind::None; }
    /// Number or Bool as double; nullopt otherwise.
    std::optional<double> asNumber() const;
    /// Bool, or a Number != 0 (USDA lets int literals stand for bools).
    std::optional<bool> asBool() const;
    /// String (tokens and strings are both strings here).
    std::optional<std::string> asString() const;
    /// Tuple or Array of numbers -> doubles; nullopt when any item is not a number.
    std::optional<std::vector<double>> asNumbers() const;
    /// Dict entry by key ("type name" keys also match by name).
    const Value* get(std::string_view key) const;

    friend bool operator==(const Value&, const Value&) = default;
};

/// An immutable, shared Value. Layer specs hold the values their layer parsed once and composed attributes point
/// at them, so a mesh asset referenced by a thousand replacements keeps one copy of its points.
class SharedValue {
public:
    SharedValue() = default;
    SharedValue(Value v) : m_v(std::make_shared<const Value>(std::move(v))) {} // NOLINT: implicit by design
    const Value& get() const noexcept { return m_v ? *m_v : noneValue(); }
    operator const Value&() const noexcept { return get(); } // NOLINT: implicit by design
    const Value* operator->() const noexcept { return &get(); }
    const Value& operator*() const noexcept { return get(); }

private:
    static const Value& noneValue() noexcept;
    std::shared_ptr<const Value> m_v;
};

/// Parses one USDA value. `err` (optional) gets "offset N: reason" on failure.
std::optional<Value> parseValue(std::string_view text, std::string* err = nullptr);

/// Canonical USDA text of a value (shortest round-trip numbers, escaped strings): the inverse of parseValue
/// up to whitespace and number spelling.
std::string formatValue(const Value& v);

/// Canonical JSON of a value (used by the flattened dump and the usd-core cross-check): Bool -> true/false,
/// Number -> number (nan/inf as strings), String -> string, Asset -> {"asset": authored, "resolved": r} where r
/// is `resolved` relative to `resolveBase` when it lies below it (else absolute), Path -> {"path": p},
/// Tuple / Array -> list, Dict -> object (keys in authored order), None -> null.
std::string valueToJson(const Value& v, std::string_view resolveBase = {});

/// Lexically normalized '/' path ("a/./b/../c" -> "a/c"; keeps a leading "/" or drive "C:/").
std::string normalizePath(std::string_view path);
/// Directory part of a '/' path ("" for a bare name).
std::string parentDir(std::string_view path);
/// Anchors `asset` to `dir` (absolute assets and URLs with "scheme:" stay as authored), normalized.
std::string anchorAssetPath(std::string_view dir, std::string_view asset);

/// JSON string literal.
std::string jsonQuote(std::string_view s);

/// Shortest decimal text that parses back to the same double.
std::string formatNumber(double v);

} // namespace fuse::relight::mods::usd
