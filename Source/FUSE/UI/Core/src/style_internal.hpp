// Internal: text helpers and value parsers shared by style.cpp (registry, cascade), style_values.cpp (transform /
// filter / clip-path / gradient / animation values) and style_parser.cpp (selectors, at-rules, sheets).
#pragma once

#include "fuse/ui/style.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace fuse::ui::detail {

inline bool isSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    return s;
}

/// A number followed by a unit ("12.5px" -> 12.5, "px"). False when `s` does not start with a number.
inline bool parseNumber(std::string_view s, f32& out, std::string_view& unit) {
    s = trim(s);
    std::size_t i = 0;
    while (i < s.size() && i < 64 && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == '-' || s[i] == '+')) ++i;
    if (i == 0) return false;
    char buf[72];
    std::memcpy(buf, s.data(), i);
    buf[i] = 0;
    char* end = nullptr;
    out = std::strtof(buf, &end);
    if (end != buf + i || !std::isfinite(out)) return false;
    unit = s.substr(i);
    return true;
}

inline bool parseLength(std::string_view s, Length& out) {
    s = trim(s);
    if (s == "auto") { out = {}; return true; }
    f32 v;
    std::string_view unit;
    if (!parseNumber(s, v, unit)) return false;
    if (unit.empty() || unit == "px" || unit == "dp") out = Length::px(v);
    else if (unit == "%") out = Length::pct(v);
    else if (unit == "fr") out = Length::fr(v);
    else return false;
    return true;
}

/// "0.2s", "150ms" or a plain number of seconds.
inline bool parseSeconds(std::string_view s, f32& out, bool allowNegative = false) {
    std::string_view unit;
    if (!parseNumber(s, out, unit)) return false;
    if (unit == "ms") out /= 1000.f;
    else if (!(unit.empty() || unit == "s")) return false;
    return allowNegative || out >= 0.f;
}
inline bool parseEasing(std::string_view s, Easing& out) {
    if (s == "linear") out = Easing::Linear;
    else if (s == "ease" || s == "ease-in-out") out = Easing::InOutCubic;
    else if (s == "ease-in") out = Easing::In;
    else if (s == "ease-out") out = Easing::Out;
    else if (s == "back" || s == "ease-out-back") out = Easing::OutBack;
    else return false;
    return true;
}
/// "90deg", "0.25turn", "1.5rad", "100grad", "0" -> radians.
inline bool parseAngle(std::string_view s, f32& radians) {
    f32 v;
    std::string_view unit;
    if (!parseNumber(s, v, unit)) return false;
    if (unit == "deg") radians = v * 0.017453292519943295f;
    else if (unit == "rad") radians = v;
    else if (unit == "turn") radians = v * 6.283185307179586f;
    else if (unit == "grad") radians = v * 0.015707963267948967f;
    else if (unit.empty() && v == 0.f) radians = 0.f;
    else return false;
    return true;
}

/// Splits `v` at `sep` outside parentheses, brackets and quotes (sep ' ' = runs of whitespace). Calls fn(piece) with
/// trimmed pieces (empty ones only between commas; none for an empty `v`). Returns false when fn does.
template <class F>
bool splitTop(std::string_view v, char sep, F&& fn) {
    std::size_t start = 0;
    int depth = 0;
    char quote = 0;
    const bool ws = sep == ' ';
    if (trim(v).empty()) return true;
    for (std::size_t i = 0; i <= v.size(); ++i) {
        const bool end = i == v.size();
        const char c = end ? sep : v[i];
        if (!end) {
            if (quote) {
                if (c == '\\') ++i;
                else if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') { quote = c; continue; }
            if (c == '(' || c == '[') { ++depth; continue; }
            if (c == ')' || c == ']') { if (depth > 0) --depth; continue; }
        }
        if (end || (depth == 0 && (ws ? isSpace(c) : c == sep))) {
            const std::string_view piece = trim(v.substr(start, i - start));
            if (!(ws && piece.empty()) && !fn(piece)) return false;
            start = i + 1;
        }
    }
    return true;
}
/// "name(args)" -> name, args. False when `s` is not one function call.
inline bool splitFunction(std::string_view s, std::string_view& name, std::string_view& args) {
    s = trim(s);
    const auto open = s.find('(');
    if (open == std::string_view::npos || open == 0 || s.back() != ')') return false;
    name = trim(s.substr(0, open));
    args = s.substr(open + 1, s.size() - open - 2);
    return true;
}

// style_values.cpp: parsers of the Raw property values; each fills `out` (type Raw) or returns false
bool parseTransformValue(std::string_view text, PropValue& out);
bool parseFilterValue(std::string_view text, PropValue& out);
bool parseClipPathValue(std::string_view text, PropValue& out);
bool parseGradientValue(std::string_view text, PropValue& out); ///< linear-gradient() / radial-gradient() / none
bool parseAnimationList(Prop p, std::string_view text, PropValue& out);
/// The decoded forms (applyProp) and their encodings (readProp).
Transform2D transformOf(const PropValue& v);
FilterList filterOf(const PropValue& v);
PropValue filterValue(const FilterList& f);
ClipShape clipShapeOf(const PropValue& v);
PropValue clipShapeValue(const ClipShape& c);
GradientSpec gradientSpecOf(const PropValue& v);
PropValue gradientSpecValue(const GradientSpec& g);
Transform2D lerpTransform(const Transform2D& a, const Transform2D& b, f32 k);
FilterList lerpFilter(const FilterList& a, const FilterList& b, f32 k);

// style_parser.cpp
/// Parses one complex selector into `rule.chain` (+ parts / sub / lists) and its specificity. False when invalid.
bool parseSelectorInto(std::string_view text, StyleRule& rule, bool& isRoot);

} // namespace fuse::ui::detail
