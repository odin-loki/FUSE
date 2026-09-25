/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/util/config/config.cpp@0867d3c and src/dxvk/rtx_render/rtx_option.cpp@0867d3c

#include <fuse/relight/options/option_value.hpp>

#include <charconv>
#include <limits>
#include <system_error>

namespace fuse::relight::options {

namespace {

bool isCSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

std::string_view skipLeadingSpace(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && isCSpace(text[i])) {
        ++i;
    }
    return text.substr(i);
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// strtoll(text, &end, 10) with a failure on "no digits" or overflow (what std::stol throws on).
bool parseSigned64(std::string_view text, std::int64_t& out) {
    text = skipLeadingSpace(text);
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    // Accumulate the magnitude as unsigned so INT64_MIN is representable.
    const std::uint64_t limit = negative ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u
                                         : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    std::uint64_t magnitude = 0;
    std::size_t digits = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            break;
        }
        const auto d = static_cast<std::uint64_t>(c - '0');
        if (magnitude > (limit - d) / 10u) {
            return false; // out of range
        }
        magnitude = magnitude * 10u + d;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    if (negative) {
        out = magnitude == limit ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(magnitude);
    } else {
        out = static_cast<std::int64_t>(magnitude);
    }
    return true;
}

bool parseFloatImpl(std::string_view text, float& out) {
    text = skipLeadingSpace(text);
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }
    float value = 0.0f;
    std::from_chars_result result{};
    bool parsed = false;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        const char* first = text.data() + 2;
        result = std::from_chars(first, text.data() + text.size(), value, std::chars_format::hex);
        parsed = result.ec == std::errc{} || result.ec == std::errc::result_out_of_range;
        // "0x" without hex digits: strtof parses the leading "0" (handled below).
    }
    if (!parsed) {
        result = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    }
    if (result.ec != std::errc{}) {
        return false;
    }
    out = negative ? -value : value;
    return true;
}

template <typename Vec, std::size_t N, typename Component>
bool parseVector(std::string_view text, Vec& out) {
    const std::vector<std::string> pieces = splitConfigList(text);
    if (pieces.empty()) {
        return false;
    }
    Component first{};
    if (!parseOptionValue(pieces[0], first)) {
        return false;
    }
    out[0] = first;
    if (pieces.size() == 1) {
        out = Vec(first); // scalar promoted to every channel
        return true;
    }
    for (std::size_t i = 1; i < N; ++i) {
        if (i >= pieces.size()) {
            return false; // missing component; earlier ones stay written (upstream behaviour)
        }
        Component c{};
        if (!parseOptionValue(pieces[i], c)) {
            return false;
        }
        out[i] = c;
    }
    return true;
}

std::string toLowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

} // namespace

OptionValue makeDefaultValue(OptionType type) {
    switch (type) {
    case OptionType::Bool: return OptionValue(std::in_place_index<0>, false);
    case OptionType::Int: return OptionValue(std::in_place_index<1>, 0);
    case OptionType::Float: return OptionValue(std::in_place_index<2>, 0.0f);
    case OptionType::Vector2: return OptionValue(std::in_place_index<3>);
    case OptionType::Vector3: return OptionValue(std::in_place_index<4>);
    case OptionType::Vector4: return OptionValue(std::in_place_index<5>);
    case OptionType::Vector2i: return OptionValue(std::in_place_index<6>);
    case OptionType::String: return OptionValue(std::in_place_index<7>);
    case OptionType::HashSet: return OptionValue(std::in_place_index<8>);
    case OptionType::HashVector: return OptionValue(std::in_place_index<9>);
    }
    return OptionValue(std::in_place_index<0>, false);
}

const char* optionTypeName(OptionType type) {
    switch (type) {
    case OptionType::Bool: return "bool";
    case OptionType::Int: return "int";
    case OptionType::Float: return "float";
    case OptionType::Vector2: return "float2";
    case OptionType::Vector3: return "float3";
    case OptionType::Vector4: return "float4";
    case OptionType::Vector2i: return "int2";
    case OptionType::String: return "string";
    case OptionType::HashSet: return "hash set";
    case OptionType::HashVector: return "hash vector";
    }
    return "unknown type";
}

bool parseOptionValue(std::string_view text, bool& out) {
    const std::string lower = toLowerAscii(text);
    if (lower == "true" || lower == "1") {
        out = true;
        return true;
    }
    if (lower == "false" || lower == "0") {
        out = false;
        return true;
    }
    return false;
}

bool parseOptionValue(std::string_view text, std::int32_t& out) {
    std::int64_t value = 0;
    if (!parseSigned64(text, value)) {
        return false;
    }
    if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    out = static_cast<std::int32_t>(value);
    return true;
}

bool parseOptionValue(std::string_view text, std::uint32_t& out) {
    std::int64_t value = 0;
    if (!parseSigned64(text, value)) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool parseOptionValue(std::string_view text, float& out) { return parseFloatImpl(text, out); }

bool parseOptionValue(std::string_view text, Vec2f& out) { return parseVector<Vec2f, 2, float>(text, out); }
bool parseOptionValue(std::string_view text, Vec3f& out) { return parseVector<Vec3f, 3, float>(text, out); }
bool parseOptionValue(std::string_view text, Vec4f& out) { return parseVector<Vec4f, 4, float>(text, out); }
bool parseOptionValue(std::string_view text, Vec2i& out) { return parseVector<Vec2i, 2, std::int32_t>(text, out); }

bool parseOptionValue(std::string_view text, std::string& out) {
    if (text.empty()) {
        return false;
    }
    out.assign(text);
    return true;
}

bool parseOptionValue(std::string_view text, std::vector<std::string>& out) {
    for (std::string& piece : splitConfigList(text)) {
        out.push_back(std::move(piece));
    }
    return true;
}

bool parseHashValue(std::string_view text, Hash64& out) {
    text = skipLeadingSpace(text);
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.size() >= 3 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') && hexDigit(text[2]) >= 0) {
        text.remove_prefix(2);
    }
    Hash64 value = 0;
    std::size_t digits = 0;
    for (char c : text) {
        const int d = hexDigit(c);
        if (d < 0) {
            break;
        }
        if (value > (std::numeric_limits<Hash64>::max() >> 4)) {
            return false; // out of range
        }
        value = (value << 4) | static_cast<Hash64>(d);
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    out = negative ? (~value + 1u) : value; // strtoull negates modulo 2^64
    return true;
}

std::size_t parseHashVector(const std::vector<std::string>& entries, HashVector& out, std::vector<std::string>* errors) {
    std::size_t failures = 0;
    for (const std::string& entry : entries) {
        Hash64 hash = 0;
        if (!parseHashValue(entry, hash)) {
            ++failures;
            if (errors) {
                errors->push_back("invalid hash '" + entry + "'");
            }
            continue;
        }
        out.push_back(hash);
    }
    return failures;
}

std::vector<std::string> splitConfigList(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t comma = text.find(',', start);
        if (comma == std::string_view::npos) {
            out.emplace_back(text.substr(start));
            break;
        }
        out.emplace_back(text.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

bool parseValueInto(std::string_view raw, OptionValue& value, std::vector<std::string>* errors) {
    bool ok = true;
    std::visit(
        [&](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, HashSetLayer>) {
                ok = v.parseFromStrings(splitConfigList(raw), errors) == 0;
                return;
            } else if constexpr (std::is_same_v<T, HashVector>) {
                ok = parseHashVector(splitConfigList(raw), v, errors) == 0;
                return;
            } else {
                ok = parseOptionValue(raw, v);
                if (!ok && errors) {
                    errors->push_back("cannot parse '" + std::string(raw) + "' as " +
                                      optionTypeName(valueType(value)));
                }
            }
        },
        value);
    return ok;
}

std::string formatOptionValue(bool value) { return value ? "True" : "False"; }
std::string formatOptionValue(std::int32_t value) { return std::to_string(value); }
std::string formatOptionValue(std::uint32_t value) { return std::to_string(value); }

std::string formatOptionValue(float value) {
    char buffer[64];
    const std::to_chars_result result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 6);
    return std::string(buffer, result.ptr);
}

std::string formatOptionValue(const Vec2f& value) {
    return formatOptionValue(value.x) + ", " + formatOptionValue(value.y);
}
std::string formatOptionValue(const Vec3f& value) {
    return formatOptionValue(value.x) + ", " + formatOptionValue(value.y) + ", " + formatOptionValue(value.z);
}
std::string formatOptionValue(const Vec4f& value) {
    return formatOptionValue(value.x) + ", " + formatOptionValue(value.y) + ", " + formatOptionValue(value.z) +
           ", " + formatOptionValue(value.w);
}
std::string formatOptionValue(const Vec2i& value) {
    return std::to_string(value.x) + ", " + std::to_string(value.y);
}
std::string formatOptionValue(const std::string& value) { return value; }
std::string formatOptionValue(const HashSetLayer& value) { return value.toString(); }

std::string formatOptionValue(const HashVector& value) {
    std::string out;
    for (Hash64 hash : value) {
        if (!out.empty()) {
            out += ", ";
        }
        out += formatHash(hash);
    }
    return out;
}

std::string formatOptionValue(const OptionValue& value) {
    return std::visit([](const auto& v) { return formatOptionValue(v); }, value);
}

} // namespace fuse::relight::options
