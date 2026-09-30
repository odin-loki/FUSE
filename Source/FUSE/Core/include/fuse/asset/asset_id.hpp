#pragma once

// UNI-U7-ASSET-1: stable 64-bit asset identity.
//
// An AssetId is the FNV-1a 64 hash of the *normalised* virtual path of a cooked asset, so the same
// asset has the same id in every process, build and platform, and ids can be stored in scenes, ECS
// components and cook manifests without a lookup table. Normalisation (normalize_asset_path):
//
//   * '\' becomes '/', runs of '/' collapse to one, a trailing '/' is dropped;
//   * ASCII letters are lower-cased (paths are case-insensitive for identity; the registry keeps the
//     spelling it was first given for the actual VFS read);
//   * "." segments are removed and ".." removes the previous segment. A leading '/' and a first
//     segment ending in ':' (a mount scheme such as "game:") are roots that ".." never removes;
//     ".." with nothing left to remove is kept (so "../x" stays "../x").
//
// Value 0 is the invalid id. The one path whose hash would be 0 is mapped to 1 (documented and
// deterministic; FNV-1a 64 reaching 0 is not expected for any real path).
//
// Text form: "asset:" followed by exactly 16 lower-case hex digits (format_asset_id). parse_asset_id
// accepts that form, "0x" + 1..16 hex digits, or 1..16 bare hex digits, in either case.
//
// Header-only and part of fuse_core so ECS components (ecs::Mesh) can hold ids without linking the
// runtime asset library (fuse_asset, Source/FUSE/Asset).

#include <fuse/types.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::asset {

struct AssetId {
    u64 value = 0;

    FUSE_HOST_DEVICE constexpr bool valid() const { return value != 0u; }
    FUSE_HOST_DEVICE constexpr explicit operator bool() const { return value != 0u; }
    FUSE_HOST_DEVICE static constexpr AssetId invalid() { return AssetId{}; }
    FUSE_HOST_DEVICE static constexpr AssetId fromValue(u64 raw) { return AssetId{raw}; }

    FUSE_HOST_DEVICE friend constexpr bool operator==(AssetId a, AssetId b) { return a.value == b.value; }
    FUSE_HOST_DEVICE friend constexpr bool operator!=(AssetId a, AssetId b) { return a.value != b.value; }
    FUSE_HOST_DEVICE friend constexpr bool operator<(AssetId a, AssetId b) { return a.value < b.value; }
};

/// std::unordered_map hasher (the id already is a hash; fold the high half in for 32-bit size_t).
struct AssetIdHash {
    std::size_t operator()(AssetId id) const noexcept {
        return static_cast<std::size_t>(id.value ^ (id.value >> 32));
    }
};

inline constexpr u64 kAssetIdFnvOffset = 14695981039346656037ull;
inline constexpr u64 kAssetIdFnvPrime = 1099511628211ull;

/// FNV-1a 64 of an already-normalised path (0 mapped to 1). constexpr, so ids of literal paths can
/// be computed at compile time: `constexpr AssetId kCube = asset_id_from_normalized("game:/cube.fusemesh");`
constexpr AssetId asset_id_from_normalized(std::string_view normalized) {
    if (normalized.empty()) {
        return AssetId{};
    }
    u64 hash = kAssetIdFnvOffset;
    for (const char c : normalized) {
        hash ^= static_cast<u64>(static_cast<unsigned char>(c));
        hash *= kAssetIdFnvPrime;
    }
    return AssetId{hash == 0u ? 1u : hash};
}

/// Normalise a virtual path (see the header comment). Empty in -> empty out.
inline std::string normalize_asset_path(std::string_view path) {
    // Split into segments on '/' or '\'.
    std::vector<std::string> segments;
    bool rooted = false;
    std::size_t start = 0;
    const std::size_t count = path.size();
    if (count > 0u && (path[0] == '/' || path[0] == '\\')) {
        rooted = true;
    }
    std::string current;
    bool schemeRoot = false;
    auto flush = [&]() {
        if (current.empty() || current == ".") {
            current.clear();
            return;
        }
        if (current == "..") {
            const std::size_t floor = schemeRoot ? 1u : 0u;
            if (segments.size() > floor && segments.back() != "..") {
                segments.pop_back();
            } else if (!rooted && !schemeRoot) {
                segments.push_back(current);
            }
            current.clear();
            return;
        }
        if (segments.empty() && !rooted && current.back() == ':') {
            schemeRoot = true;
        }
        segments.push_back(current);
        current.clear();
    };
    for (std::size_t i = start; i < count; ++i) {
        const char c = path[i];
        if (c == '/' || c == '\\') {
            flush();
            continue;
        }
        const char lowered = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        current.push_back(lowered);
    }
    flush();
    std::string out;
    if (rooted) {
        out.push_back('/');
    }
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i != 0u) {
            out.push_back('/');
        }
        out += segments[i];
    }
    return out;
}

/// AssetId of a virtual path (normalised first).
inline AssetId asset_id_of(std::string_view virtualPath) {
    return asset_id_from_normalized(normalize_asset_path(virtualPath));
}

/// "asset:0123456789abcdef".
inline std::string format_asset_id(AssetId id) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out = "asset:";
    for (int shift = 60; shift >= 0; shift -= 4) {
        out.push_back(kHex[(id.value >> static_cast<u32>(shift)) & 0xFu]);
    }
    return out;
}

/// Parses "asset:<16 hex>", "0x<1..16 hex>" or "<1..16 hex>". False (out untouched) on anything else.
inline bool parse_asset_id(std::string_view text, AssetId& out) {
    std::string_view digits = text;
    bool fixedWidth = false;
    if (digits.substr(0, 6) == "asset:") {
        digits.remove_prefix(6);
        fixedWidth = true;
    } else if (digits.substr(0, 2) == "0x" || digits.substr(0, 2) == "0X") {
        digits.remove_prefix(2);
    }
    if (digits.empty() || digits.size() > 16u || (fixedWidth && digits.size() != 16u)) {
        return false;
    }
    u64 value = 0;
    for (const char c : digits) {
        u32 nibble = 0;
        if (c >= '0' && c <= '9') {
            nibble = static_cast<u32>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            nibble = static_cast<u32>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            nibble = static_cast<u32>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | nibble;
    }
    out = AssetId{value};
    return true;
}

} // namespace fuse::asset
