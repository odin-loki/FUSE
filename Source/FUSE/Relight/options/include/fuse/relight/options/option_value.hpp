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
// Ported from dxvk-remix src/util/config/config.cpp@0867d3c (Config::parseOptionValue and
// Config::generateOptionString) and src/dxvk/rtx_render/rtx_option.cpp@0867d3c (GenericValue).
//
// Value parsing and formatting for Relight options. The rules are Remix's (which inherit DXVK's):
//
//  - bool:   "true" / "false" / "1" / "0", case-insensitive, nothing else (no trimming).
//  - int:    std::stoi rules: leading whitespace, optional sign, base-10 digits, trailing text
//            ignored; fails without digits or outside int32.
//  - float:  std::stof rules: leading whitespace, optional sign, decimal or 0x hex float, inf/nan,
//            trailing text ignored; fails without digits or when out of range.
//  - vector: comma-separated components. A single component is promoted to every channel. Missing
//            components fail, and components parsed before the failure are kept (as upstream).
//            Extra components are ignored.
//  - string: any non-empty text.
//  - hash:   std::stoull(…, 16) rules: optional `0x`, trailing text ignored.
//
// Parsing is locale-independent (std::from_chars): a game that changes the C locale cannot turn
// "0.5" into 0. Floats are written as `std::ostream << float` writes them in the "C" locale
// (printf "%g", 6 significant digits), so files written here match files Remix writes.
#pragma once

#include <fuse/relight/options/hash_set_layer.hpp>
#include <fuse/relight/options/option_types.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fuse::relight::options {

/// Type-erased option value. Alternative index == static_cast<size_t>(OptionType).
using OptionValue =
    std::variant<bool, std::int32_t, float, Vec2f, Vec3f, Vec4f, Vec2i, std::string, HashSetLayer, HashVector>;

static_assert(std::variant_size_v<OptionValue> == static_cast<std::size_t>(OptionType::HashVector) + 1,
              "OptionValue alternatives must follow OptionType");

/// Zero / empty value of `type`.
OptionValue makeDefaultValue(OptionType type);

inline OptionType valueType(const OptionValue& value) { return static_cast<OptionType>(value.index()); }

/// Short type name used in the generated docs ("bool", "int", "float", "float2", "float3", "float4",
/// "int2", "string", "hash set", "hash vector").
const char* optionTypeName(OptionType type);

// ----------------------------------------------------------------------------
// Parsing. Each returns false on failure; see the header comment for what is kept on failure.
// ----------------------------------------------------------------------------

bool parseOptionValue(std::string_view text, bool& out);
bool parseOptionValue(std::string_view text, std::int32_t& out);
/// std::stol rules, narrowed to 32 bits (so "-1" gives 0xFFFFFFFF).
bool parseOptionValue(std::string_view text, std::uint32_t& out);
bool parseOptionValue(std::string_view text, float& out);
bool parseOptionValue(std::string_view text, Vec2f& out);
bool parseOptionValue(std::string_view text, Vec3f& out);
bool parseOptionValue(std::string_view text, Vec4f& out);
bool parseOptionValue(std::string_view text, Vec2i& out);
bool parseOptionValue(std::string_view text, std::string& out);
/// Splits on ',' and appends the (untrimmed) pieces. Always succeeds.
bool parseOptionValue(std::string_view text, std::vector<std::string>& out);

/// One hash, std::stoull(text, nullptr, 16) rules.
bool parseHashValue(std::string_view text, Hash64& out);

/// Hash-vector entries (no `-` entries, order kept). Bad entries are skipped and counted.
std::size_t parseHashVector(const std::vector<std::string>& entries, HashVector& out,
                            std::vector<std::string>* errors = nullptr);

/// Split like repeated std::getline(stream, piece, ','): no trailing empty piece, empty input gives
/// no pieces, inner empty pieces are kept.
std::vector<std::string> splitConfigList(std::string_view text);

/// Parse `raw` into `value`, keeping its current alternative. This is the upstream readValue rule:
/// the result starts from the current value, and a failed parse leaves whatever was written before
/// the failure. Returns false when anything failed to parse; `errors` receives details.
bool parseValueInto(std::string_view raw, OptionValue& value, std::vector<std::string>* errors = nullptr);

// ----------------------------------------------------------------------------
// Formatting
// ----------------------------------------------------------------------------

std::string formatOptionValue(bool value);          ///< "True" / "False"
std::string formatOptionValue(std::int32_t value);
std::string formatOptionValue(std::uint32_t value);
std::string formatOptionValue(float value);         ///< "%g" with 6 significant digits, "C" locale
std::string formatOptionValue(const Vec2f& value);  ///< "x, y"
std::string formatOptionValue(const Vec3f& value);
std::string formatOptionValue(const Vec4f& value);
std::string formatOptionValue(const Vec2i& value);
std::string formatOptionValue(const std::string& value);
std::string formatOptionValue(const HashSetLayer& value);
std::string formatOptionValue(const HashVector& value); ///< "0x…, 0x…" in stored order
std::string formatOptionValue(const OptionValue& value);

} // namespace fuse::relight::options
