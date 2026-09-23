// FUSE Relight: hash <-> text conversions used by Remix mods, rtx.conf and captures.
//
// Behaviour matches dxvk-remix @0867d3c:
//   * hashToString (rtx_utils.h): 16 upper-case hex digits, zero-padded, no prefix. USD prim names
//     are "<prefix><hashToString(h)>" (lssusd game_exporter_paths.h prefixes below);
//   * rtx.conf hash lists (rtx_option.cpp) write "0x" + 16 upper-case digits and read with
//     std::stoull(s, nullptr, 16);
//   * getNamedHash (rtx_mod_usd.cpp) reads prim names with std::strtoull(name + len(prefix), 16).
#pragma once

#include <fuse/relight/hash/xxh.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace fuse::relight::hash {

namespace prim_prefix {
inline constexpr std::string_view kMesh = "mesh_";
inline constexpr std::string_view kSkeleton = "skel_";
inline constexpr std::string_view kLight = "light_";
inline constexpr std::string_view kMaterial = "mat_";
} // namespace prim_prefix

/// hashToString: "0123456789ABCDEF" style, always 16 characters.
[[nodiscard]] std::string hashToString(Hash64 hash);

/// rtx.conf style: "0x" + hashToString(hash).
[[nodiscard]] std::string hashToOptionString(Hash64 hash);

/// "<prefix><hashToString(hash)>", e.g. primName(prim_prefix::kMesh, h) = "mesh_0123...".
[[nodiscard]] std::string primName(std::string_view prefix, Hash64 hash);

/// std::stoull(text, nullptr, 16) as on Windows: leading whitespace, an optional sign ('-' negates
/// modulo 2^64), an optional 0x/0X, then hex digits up to the first non-hex character. nullopt where
/// stoull throws: no digits (std::invalid_argument; a "0x" prefix without a hex digit after it
/// counts as no digits, as in the Windows CRT) or a value above 2^64 - 1 (std::out_of_range).
[[nodiscard]] std::optional<Hash64> parseHashOption(std::string_view text);

/// getNamedHash: when `name` starts with `prefix`, std::strtoull(rest, nullptr, 16) (0 without
/// digits, 2^64 - 1 on overflow); otherwise 0 ("not a replacement").
[[nodiscard]] Hash64 hashFromPrimName(std::string_view name, std::string_view prefix);

} // namespace fuse::relight::hash
