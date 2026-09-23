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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_option_constants.h@0867d3c
//
// Relight options (RL-0.6): shared value types, flags, layer keys and constants.
//
// Behaviour follows upstream Remix's RtxOption system (documentation/RemixConfig.md). The names are
// FUSE-native: RtxOption<T> -> Option<T>, RtxOptionLayer -> OptionLayer, RtxOptionManager ->
// OptionManager, RtxOptionFlags -> OptionFlags. Priorities, file names and environment variable
// names are Remix's, so rtx.conf / user.conf / dxvk.conf files and launch scripts work unchanged.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fuse::relight::options {

// ============================================================================
// Value types
// ============================================================================

/// 64-bit asset hash (XXH64 in Remix). Written as `0x` + 16 upper-case hex digits.
using Hash64 = std::uint64_t;
/// Resolved value of a hash-set option (Remix `fast_unordered_set`).
using HashSet = std::unordered_set<Hash64>;
/// Ordered hash list option (Remix `std::vector<XXH64_hash_t>`). Does not merge across layers.
using HashVector = std::vector<Hash64>;

struct Vec2f {
    float x = 0.0f;
    float y = 0.0f;
    constexpr Vec2f() = default;
    constexpr explicit Vec2f(float s) : x(s), y(s) {}
    constexpr Vec2f(float x_, float y_) : x(x_), y(y_) {}
    float& operator[](std::size_t i) { return i == 0 ? x : y; }
    float operator[](std::size_t i) const { return i == 0 ? x : y; }
    friend constexpr bool operator==(const Vec2f& a, const Vec2f& b) { return a.x == b.x && a.y == b.y; }
    friend constexpr bool operator!=(const Vec2f& a, const Vec2f& b) { return !(a == b); }
};

struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    constexpr Vec3f() = default;
    constexpr explicit Vec3f(float s) : x(s), y(s), z(s) {}
    constexpr Vec3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    float& operator[](std::size_t i) { return i == 0 ? x : (i == 1 ? y : z); }
    float operator[](std::size_t i) const { return i == 0 ? x : (i == 1 ? y : z); }
    friend constexpr bool operator==(const Vec3f& a, const Vec3f& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    friend constexpr bool operator!=(const Vec3f& a, const Vec3f& b) { return !(a == b); }
};

struct Vec4f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
    constexpr Vec4f() = default;
    constexpr explicit Vec4f(float s) : x(s), y(s), z(s), w(s) {}
    constexpr Vec4f(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    float& operator[](std::size_t i) { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    float operator[](std::size_t i) const { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    friend constexpr bool operator==(const Vec4f& a, const Vec4f& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }
    friend constexpr bool operator!=(const Vec4f& a, const Vec4f& b) { return !(a == b); }
};

struct Vec2i {
    std::int32_t x = 0;
    std::int32_t y = 0;
    constexpr Vec2i() = default;
    constexpr explicit Vec2i(std::int32_t s) : x(s), y(s) {}
    constexpr Vec2i(std::int32_t x_, std::int32_t y_) : x(x_), y(y_) {}
    std::int32_t& operator[](std::size_t i) { return i == 0 ? x : y; }
    std::int32_t operator[](std::size_t i) const { return i == 0 ? x : y; }
    friend constexpr bool operator==(const Vec2i& a, const Vec2i& b) { return a.x == b.x && a.y == b.y; }
    friend constexpr bool operator!=(const Vec2i& a, const Vec2i& b) { return !(a == b); }
};

/// Storage type of an option. The order matches the alternatives of OptionValue (option_value.hpp).
/// Enums and every integer width are stored as Int (32-bit), exactly as in Remix.
enum class OptionType : std::uint8_t {
    Bool = 0,
    Int,
    Float,
    Vector2,
    Vector3,
    Vector4,
    Vector2i,
    String,
    HashSet,    ///< Merges across layers (positive and `-` negative entries).
    HashVector, ///< Does not merge: the strongest active layer wins. Order and length are kept.
};

/// Float and float-vector options blend across layers; every other type uses a threshold.
constexpr bool isBlendableType(OptionType type) {
    return type == OptionType::Float || type == OptionType::Vector2 || type == OptionType::Vector3 ||
           type == OptionType::Vector4;
}

// ============================================================================
// Flags
// ============================================================================

/// Option flags (Remix RtxOptionFlags). Combine with `|`; the result is a plain std::uint32_t.
///  - NoSave: runtime-only. Always routed to the Derived layer and never written to a config file.
///  - NoReset: kept when a layer is cleared or disabled (not when it is released).
///  - UserSetting: end-user preference. Belongs in user.conf / the Quality layer, not in rtx.conf.
///  - InvalidatesDrawcallTranslation: a change forces preserved draw calls to be retranslated.
struct OptionFlags {
    enum : std::uint32_t {
        NoSave = 0x1,
        NoReset = 0x2,
        UserSetting = 0x4,
        InvalidatesDrawcallTranslation = 0x8,
    };
};

/// Flags that decide which layer an option belongs in (layer migration). NoSave and NoReset are
/// orthogonal and not part of it.
inline constexpr std::uint32_t kOptionCategoryFlags = OptionFlags::UserSetting;

// ============================================================================
// Environment variables and file names (Remix-compatible)
// ============================================================================

inline constexpr const char* kDxvkConfEnvVar = "DXVK_CONFIG_FILE";         ///< dxvk.conf path(s), comma-separated
inline constexpr const char* kRtxConfEnvVar = "DXVK_RTX_CONFIG_FILE";      ///< rtx.conf path(s), comma-separated
inline constexpr const char* kAppConfigExeEnvVar = "DXVK_USE_CONF_FOR_EXE"; ///< exe path used for app-config matching

inline constexpr const char* kDxvkConfFileName = "dxvk.conf";
inline constexpr const char* kRtxConfFileName = "rtx.conf";
inline constexpr const char* kUserConfFileName = "user.conf";

// ============================================================================
// Layer priorities and blend constants
// ============================================================================

/// Dynamic (component-managed) layers use priorities in [min, max]. System layers sit outside it.
inline constexpr std::uint32_t kMinDynamicLayerPriority = 100;
/// 10,000,000 keeps priorities exact when a Logic graph passes them as float (24-bit mantissa).
inline constexpr std::uint32_t kMaxDynamicLayerPriority = 10000000;
inline constexpr std::uint32_t kDefaultDynamicLayerPriority = 10000;

/// Blend strength requests use MAX, so the "no request" sentinel sits below [0, 1].
inline constexpr float kEmptyBlendStrengthRequest = -1.0f;
/// Blend threshold requests use MIN, so the "no request" sentinel sits above [0, 1].
inline constexpr float kEmptyBlendThresholdRequest = 2.0f;

/// Default blend strength and threshold of system and file layers (Remix: 1.0 / 0.1).
inline constexpr float kDefaultLayerBlendStrength = 1.0f;
inline constexpr float kDefaultLayerBlendThreshold = 0.1f;

// ============================================================================
// Layer keys
// ============================================================================

/// Compile-time description of a system layer (priority + display name).
struct SystemLayerId {
    std::uint32_t priority;
    const char* name;
};

inline constexpr SystemLayerId kDefaultLayerId = {0, "Default Values"};
inline constexpr SystemLayerId kDxvkConfLayerId = {1, "DXVK Config"};
inline constexpr SystemLayerId kAppConfigLayerId = {2, "Hardcoded EXE Config"};
inline constexpr SystemLayerId kRtxConfLayerId = {3, "Remix Config"};
inline constexpr SystemLayerId kBaseGameModLayerId = {4, "baseGameMod Remix Config"};
inline constexpr SystemLayerId kEnvironmentLayerId = {5, "Environment Variable Overrides"};
inline constexpr SystemLayerId kDerivedLayerId = {6, "Derived Settings"};
inline constexpr SystemLayerId kUserLayerId = {0xFFFFFFFEu, "User Settings"};
inline constexpr SystemLayerId kQualityLayerId = {0xFFFFFFFFu, "Quality Presets"};

/// Identifies a layer. Higher priority overrides lower; equal priorities order by name, and the
/// alphabetically earlier name wins (`a.conf` overrides `z.conf`). `operator<` sorts the strongest
/// layer first, so iterating a std::map<OptionLayerKey, ...> visits layers from strongest to weakest.
struct OptionLayerKey {
    std::uint32_t priority = 0;
    std::string name;

    OptionLayerKey() = default;
    OptionLayerKey(std::uint32_t priority_, std::string name_) : priority(priority_), name(std::move(name_)) {}
    OptionLayerKey(const SystemLayerId& id) : priority(id.priority), name(id.name) {} // NOLINT(implicit)

    bool operator<(const OptionLayerKey& other) const {
        if (priority != other.priority) {
            return priority > other.priority;
        }
        return name < other.name;
    }
    bool operator==(const OptionLayerKey& other) const {
        return priority == other.priority && name == other.name;
    }
    bool operator!=(const OptionLayerKey& other) const { return !(*this == other); }

    /// "'<name>' (priority: <n>)", as Remix prints it.
    std::string toString() const { return "'" + name + "' (priority: " + std::to_string(priority) + ")"; }
};

} // namespace fuse::relight::options
