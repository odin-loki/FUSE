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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_option.h@0867d3c
//
// Typed, layered options (Remix RtxOption<T>). Declare options inside a class with the macros at the
// end of this file:
//
//   struct MyOptions {
//       FUSE_RELIGHT_OPTION("rtx", bool, enableRaytracing, true, "Enable ray tracing.");
//       FUSE_RELIGHT_OPTION_ARGS("rtx", float, exposure, 1.0f, "Exposure value.",
//           args.minValue = 0.0f;
//           args.maxValue = 10.0f;
//           args.onChangeCallback = &onExposureChanged);
//       FUSE_RELIGHT_OPTION_FLAG("relight", int32_t, debugView, 0, OptionFlags::NoSave, "Debug view.");
//   };
//
//   if (MyOptions::enableRaytracing()) { … }
//   MyOptions::exposure.setDeferred(2.0f);   // resolved at the end of the frame
//
// Values live in sparse layers (option_layer.hpp) and are resolved once per frame by
// OptionManager::applyPendingValues(): floats and float vectors blend by layer strength, other types
// take the strongest active layer, hash sets merge with `-` removal. Reads return the value
// resolved at the last applyPendingValues() (or setImmediately()).
//
// Differences from upstream (all deliberate):
//  - Values are a std::variant (OptionValue) instead of a union with raw new/delete.
//  - onChange callbacks receive the `void* context` given to applyPendingValues (upstream passes
//    the DxvkDevice*); it is nullptr during startup, as upstream.
//  - Every option also answers to its relight.* / rtx.* twin name and to explicit aliases
//    (OptionManager::addAlias) when reading config files and looking options up by name.
//  - Keybind (VirtualKeys) options are not ported; they belong with the overlay (RL-6.1).
#pragma once

#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_types.hpp>
#include <fuse/relight/options/option_value.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fuse::relight::options {

class OptionLayer;
class OptionManager;

namespace detail {
/// The single lock that guards every option, layer and the registries (Remix getUpdateMutex()).
std::recursive_mutex& optionMutex();
} // namespace detail

/// onChange callback. `context` is what the frame loop passes to OptionManager::applyPendingValues()
/// (nullptr for the startup pass).
using OptionChangeCallback = void (*)(void* context);

// ============================================================================
// Invalidation scope
// ============================================================================

/// RAII scope: options read inside it are tagged with `requiredFlags` (merged with any enclosing
/// scope's flags). Used to find options that feed draw-call translation.
class OptionInvalidationScope {
public:
    explicit OptionInvalidationScope(std::uint32_t requiredFlags) noexcept : m_savedFlags(s_requiredFlags) {
        s_requiredFlags |= requiredFlags;
    }
    ~OptionInvalidationScope() noexcept { s_requiredFlags = m_savedFlags; }
    OptionInvalidationScope(const OptionInvalidationScope&) = delete;
    OptionInvalidationScope& operator=(const OptionInvalidationScope&) = delete;

    /// Flags of the active scopes on this thread; 0 when none is active.
    static std::uint32_t getRequiredFlags() noexcept { return s_requiredFlags; }

private:
    std::uint32_t m_savedFlags;
    inline static thread_local std::uint32_t s_requiredFlags = 0;
};

#define FUSE_RELIGHT_OPTION_SCOPE_NAME_(line) fuseRelightOptionInvalidationScope_##line
#define FUSE_RELIGHT_OPTION_SCOPE_NAME(line) FUSE_RELIGHT_OPTION_SCOPE_NAME_(line)
#define FUSE_RELIGHT_OPTION_INVALIDATION_SCOPE(flags)                                                              \
    [[maybe_unused]] const ::fuse::relight::options::OptionInvalidationScope FUSE_RELIGHT_OPTION_SCOPE_NAME(       \
        __LINE__) {                                                                                                \
        static_cast<std::uint32_t>(flags)                                                                          \
    }

// ============================================================================
// Declaration arguments
// ============================================================================

template <typename T>
struct OptionArgs {
    /// Environment variable that overrides the option (Environment layer, priority 5).
    const char* environment = nullptr;
    std::uint32_t flags = 0;
    std::optional<T> minValue;
    std::optional<T> maxValue;
    /// Called when the resolved value changes (and once at startup).
    OptionChangeCallback onChangeCallback = nullptr;
};

// ============================================================================
// OptionBase: type-erased option (Remix RtxOptionImpl)
// ============================================================================

class OptionBase {
    friend class OptionManager;
    friend class OptionLayer;

public:
    /// One layer's value, with the blend settings captured from the layer.
    struct LayerValue {
        OptionValue value;
        float blendStrength = 1.0f;
        float blendThreshold = 0.5f;
    };
    /// Strongest layer first.
    using LayerValueMap = std::map<OptionLayerKey, LayerValue>;

    OptionBase(const OptionBase&) = delete;
    OptionBase& operator=(const OptionBase&) = delete;
    virtual ~OptionBase();

    /// "<category>.<name>", e.g. "rtx.enableRaytracing".
    const std::string& getFullName() const { return m_fullName; }
    const std::string& getName() const { return m_name; }
    const std::string& getCategory() const { return m_category; }
    const char* getDescription() const { return m_description; }
    /// Environment variable name, or nullptr.
    const char* getEnvironmentVariable() const { return m_environment; }
    OptionType getType() const { return m_type; }
    const char* getTypeString() const { return optionTypeName(m_type); }
    std::uint32_t getFlags() const { return m_flags.load(std::memory_order_relaxed); }
    bool hasOnChangeCallback() const { return m_onChange != nullptr; }

    /// OR the active invalidation scope's flags into this option (called by every value read).
    void tagInvalidationScope() const {
        if (const std::uint32_t scopeFlags = OptionInvalidationScope::getRequiredFlags()) {
            if ((m_flags.load(std::memory_order_relaxed) & scopeFlags) != scopeFlags) {
                m_flags.fetch_or(scopeFlags, std::memory_order_relaxed);
            }
        }
    }

    /// Layer a write goes to. NoSave options always go to the Derived layer; otherwise an explicit
    /// layer wins; otherwise the thread's OptionLayerTarget and the UserSetting flag decide:
    ///   User target:    UserSetting -> User layer,  else -> Remix Config (rtx.conf) layer
    ///   Derived target: UserSetting -> Quality layer (User layer while the graphics preset is
    ///                   Custom),                    else -> Derived layer
    const OptionLayer* getTargetLayer(const OptionLayer* explicitLayer = nullptr) const;

    /// Resolved value equals the Default Values layer's value.
    bool isDefault() const;
    /// The layer holds a value (for hash sets: a non-empty set, or an opinion about `hash`).
    bool hasValueInLayer(const OptionLayer* layer, std::optional<Hash64> hash = std::nullopt) const;
    /// Copy of the layer's value, if any.
    std::optional<OptionValue> getLayerValue(const OptionLayer* layer) const;
    std::string valueToString(const OptionValue& value) const { return formatOptionValue(value); }
    std::string getResolvedValueAsString() const;
    /// Copy of the resolved value.
    OptionValue getResolvedValue() const;
    std::optional<OptionValue> getMinValueGeneric() const;
    std::optional<OptionValue> getMaxValueGeneric() const;
    /// Copy of every layer value (strongest first).
    LayerValueMap getLayerValues() const;

    /// Insert this option's value from `layer`'s config, when the config has one.
    void readOptionLayer(const OptionLayer& layer);
    /// Parse this option from `config` into `layer`'s existing value (created if missing).
    void readOption(const OptionConfig& config, const OptionLayer* layer);
    /// Load the declared environment variable into `envLayer`. Returns true when it was set and parsed.
    bool loadFromEnvironmentVariable(const OptionLayer* envLayer, std::string* outValue = nullptr);
    /// Remove the layer's value.
    void disableLayerValue(const OptionLayer* layer);
    void updateLayerBlendStrength(const OptionLayer& layer);
    /// Move the value from one layer to another (hash sets merge; other types overwrite).
    void moveLayerValue(const OptionLayer* sourceLayer, const OptionLayer* destinationLayer);
    /// Remove values (or one hash's opinions) from every layer stronger than `targetLayer`.
    void clearFromStrongerLayers(const OptionLayer* targetLayer = nullptr, std::optional<Hash64> hash = std::nullopt);
    /// Strongest active layer above the target layer that holds a value (it would hide an edit).
    const OptionLayer* getBlockingLayer(const OptionLayer* targetLayer = nullptr,
                                        std::optional<Hash64> hash = std::nullopt) const;
    /// Visit layers holding a value, strongest first; return false from the callback to stop.
    /// Non-float layers below their threshold are skipped unless `includeInactiveLayers`.
    void forEachLayerValue(const std::function<bool(const OptionLayer*, const OptionValue&)>& callback,
                           std::optional<Hash64> hash = std::nullopt, bool includeInactiveLayers = false) const;

    void markDirty();
    bool isDirty() const;
    void invokeOnChangeCallback(void* context) const;

    /// Copy every non-default layer value to `destination` through `transform`
    /// (src, dst, destinationHadValue) -> migrated. Returns true when anything migrated.
    bool migrateValuesTo(OptionBase* destination,
                         const std::function<bool(const OptionValue& source, OptionValue& destination,
                                                  bool destinationHadValue)>& transform);

    /// Config keys this option reads, in lookup order: its full name, its relight.* <-> rtx.* twin,
    /// then explicit aliases (OptionManager::addAlias).
    std::vector<std::string> getConfigKeys() const;
    /// The first non-empty value for one of getConfigKeys() in `config`.
    const std::string* findConfigValue(const OptionConfig& config, std::string* keyUsed = nullptr) const;

    /// True when removing `layer`'s value would not change what the weaker layers resolve to.
    bool isLayerValueRedundant(const OptionLayer* layer) const;

protected:
    OptionBase(const char* category, const char* name, OptionType type, const char* description,
               const char* environment, std::uint32_t flags, OptionChangeCallback onChange);

    /// Register in the global registry and insert the default into the Default Values layer.
    /// Returns false when an option with the same full name exists (this one stays unregistered).
    bool registerAndInitialize(OptionValue defaultValue);

    /// Refresh the typed copy of m_resolved (Option<T>).
    virtual void onResolvedValueChanged() = 0;

    // Everything below expects detail::optionMutex() to be held.
    const OptionValue* findLayerValue(const OptionLayer* layer) const;
    OptionValue* getOrCreateLayerValue(const OptionLayer* layer, bool* created = nullptr);
    void insertLayerValue(const OptionValue& value, const OptionLayer* layer);
    bool resolveInto(OptionValue& out, const OptionLayer* excludeLayer = nullptr) const;
    bool resolveNow();
    bool clampValue(OptionValue& value) const;
    void writeOption(OptionConfig& config, const OptionLayer* layer, bool changedOptionOnly) const;
    bool setBound(std::optional<OptionValue>& slot, const OptionValue& value);

    std::string m_category;
    std::string m_name;
    std::string m_fullName;
    const char* m_description;
    const char* m_environment;
    OptionType m_type;
    mutable std::atomic<std::uint32_t> m_flags;
    OptionChangeCallback m_onChange;
    OptionValue m_resolved;
    std::optional<OptionValue> m_minValue;
    std::optional<OptionValue> m_maxValue;
    LayerValueMap m_layerValues;
    bool m_registered = false;
};

// ============================================================================
// Type mapping
// ============================================================================

namespace detail {

template <typename T>
inline constexpr bool kIsIntLike = (std::is_integral_v<T> && !std::is_same_v<T, bool>) || std::is_enum_v<T>;

template <typename T>
inline constexpr bool kIsClampable = kIsIntLike<T> || std::is_same_v<T, float> || std::is_same_v<T, Vec2f> ||
                                     std::is_same_v<T, Vec3f> || std::is_same_v<T, Vec4f> || std::is_same_v<T, Vec2i>;

template <typename T>
inline constexpr bool kAlwaysFalse = false;

template <typename T>
constexpr OptionType optionTypeOf() {
    if constexpr (std::is_same_v<T, bool>) {
        return OptionType::Bool;
    } else if constexpr (kIsIntLike<T>) {
        return OptionType::Int;
    } else if constexpr (std::is_same_v<T, float>) {
        return OptionType::Float;
    } else if constexpr (std::is_same_v<T, Vec2f>) {
        return OptionType::Vector2;
    } else if constexpr (std::is_same_v<T, Vec3f>) {
        return OptionType::Vector3;
    } else if constexpr (std::is_same_v<T, Vec4f>) {
        return OptionType::Vector4;
    } else if constexpr (std::is_same_v<T, Vec2i>) {
        return OptionType::Vector2i;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return OptionType::String;
    } else if constexpr (std::is_same_v<T, HashSet>) {
        return OptionType::HashSet;
    } else if constexpr (std::is_same_v<T, HashVector>) {
        return OptionType::HashVector;
    } else {
        static_assert(kAlwaysFalse<T>, "Unsupported Relight option type");
        return OptionType::Bool;
    }
}

template <typename T>
OptionValue toOptionValue(const T& value) {
    if constexpr (kIsIntLike<T>) {
        // Every integer width and enum is stored as int32, as upstream stores them in an int.
        return OptionValue(std::in_place_index<static_cast<std::size_t>(OptionType::Int)>,
                           static_cast<std::int32_t>(value));
    } else if constexpr (std::is_same_v<T, HashSet>) {
        HashSetLayer layer;
        layer.assignPositives(value);
        return OptionValue(std::in_place_index<static_cast<std::size_t>(OptionType::HashSet)>, std::move(layer));
    } else {
        return OptionValue(std::in_place_index<static_cast<std::size_t>(optionTypeOf<T>())>, value);
    }
}

template <typename T>
T fromOptionValue(const OptionValue& value) {
    if constexpr (kIsIntLike<T>) {
        return static_cast<T>(std::get<std::int32_t>(value));
    } else if constexpr (std::is_same_v<T, HashSet>) {
        return std::get<HashSetLayer>(value).positives();
    } else {
        return std::get<T>(value);
    }
}

} // namespace detail

// ============================================================================
// Option<T> (Remix RtxOption<T>)
// ============================================================================

/// Supported T: bool, any integer type or enum (stored as int32), float, Vec2f, Vec3f, Vec4f,
/// Vec2i, std::string, HashSet (merging hash set), HashVector (ordered hash list).
template <typename T>
class Option final : public OptionBase {
public:
    using ValueType = T;

    Option(const char* category, const char* name, const T& defaultValue, const char* description = "",
           OptionArgs<T> args = {})
        : OptionBase(category, name, detail::optionTypeOf<T>(), description, args.environment, args.flags,
                     args.onChangeCallback),
          m_cached(defaultValue) {
        if constexpr (std::is_same_v<T, HashSet>) {
            assert(defaultValue.empty() && "Hash set options must have empty {} defaults");
        }
        if (registerAndInitialize(detail::toOptionValue(defaultValue))) {
            if constexpr (detail::kIsClampable<T>) {
                if (args.minValue.has_value()) {
                    setMinValue(*args.minValue);
                }
                if (args.maxValue.has_value()) {
                    setMaxValue(*args.maxValue);
                }
            } else {
                assert(!args.minValue.has_value() && !args.maxValue.has_value() &&
                       "minValue/maxValue are only supported on numeric and vector options");
            }
        }
    }

    const T& operator()() const { return get(); }

    /// Resolved value.
    const T& get() const {
        std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
        tagInvalidationScope();
        return m_cached;
    }

    /// Resolved value without taking the lock; the caller must hold detail::optionMutex().
    const T& getNoLock() const {
        tagInvalidationScope();
        return m_cached;
    }

    /// Write `value` to a layer (explicit, or the target layer); it is resolved at the next
    /// OptionManager::applyPendingValues(). For hash sets this replaces the layer's positive entries.
    void setDeferred(const T& value, const OptionLayer* layer = nullptr) { setValue(value, layer, false); }

    /// Like setDeferred(), then resolve immediately so reads this frame see the value. The option is
    /// marked dirty as upstream, but it has already resolved, so the end-of-frame pass sees no change
    /// and does not run its onChange callback (upstream behaves the same).
    void setImmediately(const T& value, const OptionLayer* layer = nullptr) { setValue(value, layer, true); }

    /// Hash sets: add `hash` in a layer (overrides a weaker layer's removal).
    template <typename U = T, std::enable_if_t<std::is_same_v<U, HashSet>, int> = 0>
    void addHash(Hash64 hash, const OptionLayer* layer = nullptr) {
        editHashSet(layer, [hash](HashSetLayer& set) { set.add(hash); }, false);
    }

    /// Hash sets: remove `hash` in a layer (a `-0x…` entry that overrides weaker layers).
    template <typename U = T, std::enable_if_t<std::is_same_v<U, HashSet>, int> = 0>
    void removeHash(Hash64 hash, const OptionLayer* layer = nullptr) {
        editHashSet(layer, [hash](HashSetLayer& set) { set.remove(hash); }, false);
    }

    /// Hash sets: drop the layer's opinion about `hash` (the layer entry goes when it empties).
    template <typename U = T, std::enable_if_t<std::is_same_v<U, HashSet>, int> = 0>
    void clearHash(Hash64 hash, const OptionLayer* layer = nullptr) {
        editHashSet(layer, [hash](HashSetLayer& set) { set.clear(hash); }, true);
    }

    /// Hash sets: `hash` is in the resolved set.
    template <typename U = T, std::enable_if_t<std::is_same_v<U, HashSet>, int> = 0>
    bool containsHash(Hash64 hash) const {
        std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
        tagInvalidationScope();
        return std::get<HashSetLayer>(m_resolved).count(hash) > 0;
    }

    /// Value in the Default Values layer.
    T getDefaultValue() const;

    /// Write the default value to the target layer.
    void resetToDefault() { setDeferred(getDefaultValue()); }

    OptionType getOptionType() const { return detail::optionTypeOf<T>(); }

    template <typename U = T, std::enable_if_t<detail::kIsClampable<U>, int> = 0>
    void setMinValue(const T& value) {
        std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
        if (setBound(m_minValue, detail::toOptionValue(value))) {
            markDirty();
        }
    }

    template <typename U = T, std::enable_if_t<detail::kIsClampable<U>, int> = 0>
    std::optional<T> getMinValue() const {
        std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
        return m_minValue ? std::optional<T>(detail::fromOptionValue<T>(*m_minValue)) : std::nullopt;
    }

    template <typename U = T, std::enable_if_t<detail::kIsClampable<U>, int> = 0>
    void setMaxValue(const T& value) {
        std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
        if (setBound(m_maxValue, detail::toOptionValue(value))) {
            markDirty();
        }
    }

    template <typename U = T, std::enable_if_t<detail::kIsClampable<U>, int> = 0>
    std::optional<T> getMaxValue() const {
        std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
        return m_maxValue ? std::optional<T>(detail::fromOptionValue<T>(*m_maxValue)) : std::nullopt;
    }

private:
    void onResolvedValueChanged() override { m_cached = detail::fromOptionValue<T>(m_resolved); }

    void setValue(const T& value, const OptionLayer* layer, bool immediately);

    template <typename Edit>
    void editHashSet(const OptionLayer* layer, Edit&& edit, bool dropWhenEmpty);

    T m_cached;
};

} // namespace fuse::relight::options

// Out-of-line members need OptionLayer (onLayerValueChanged) and the default layer accessor.
#include <fuse/relight/options/option_layer.hpp>

namespace fuse::relight::options {

template <typename T>
T Option<T>::getDefaultValue() const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const OptionValue* value = findLayerValue(OptionLayer::getDefaultLayer());
    assert(value != nullptr && "the default value is set at construction");
    return value ? detail::fromOptionValue<T>(*value) : m_cached;
}

template <typename T>
void Option<T>::setValue(const T& value, const OptionLayer* layer, bool immediately) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const OptionLayer* targetLayer = getTargetLayer(layer);
    if (!targetLayer) {
        return;
    }
    OptionValue* layerValue = getOrCreateLayerValue(targetLayer);
    if (!layerValue) {
        return;
    }
    if constexpr (std::is_same_v<T, HashSet>) {
        std::get<HashSetLayer>(*layerValue).assignPositives(value);
    } else {
        *layerValue = detail::toOptionValue(value);
    }
    targetLayer->onLayerValueChanged();
    if (immediately) {
        resolveNow();
    }
    markDirty();
}

template <typename T>
template <typename Edit>
void Option<T>::editHashSet(const OptionLayer* layer, Edit&& edit, bool dropWhenEmpty) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const OptionLayer* targetLayer = getTargetLayer(layer);
    if (!targetLayer) {
        return;
    }
    OptionValue* layerValue = getOrCreateLayerValue(targetLayer);
    if (!layerValue) {
        return;
    }
    HashSetLayer& set = std::get<HashSetLayer>(*layerValue);
    edit(set);
    targetLayer->onLayerValueChanged();
    if (dropWhenEmpty && set.empty()) {
        disableLayerValue(targetLayer);
    }
    markDirty();
}

} // namespace fuse::relight::options

// ============================================================================
// Declaration macros (Remix RTX_OPTION*). Use inside a class or struct body.
// ============================================================================

#define FUSE_RELIGHT_OPTION_FULL(category, type, name, value, description, ...)                                    \
public:                                                                                                            \
    inline static ::fuse::relight::options::Option<type> name = ::fuse::relight::options::Option<type>(            \
        category, #name, value, description, []() {                                                                 \
            ::fuse::relight::options::OptionArgs<type> args;                                                        \
            __VA_ARGS__;                                                                                            \
            return args;                                                                                            \
        }());                                                                                                       \
    static ::fuse::relight::options::Option<type>& name##Object() { return name; }

#define FUSE_RELIGHT_OPTION(category, type, name, value, description)                                              \
    FUSE_RELIGHT_OPTION_FULL(category, type, name, value, description, {})
#define FUSE_RELIGHT_OPTION_ARGS(category, type, name, value, description, ...)                                    \
    FUSE_RELIGHT_OPTION_FULL(category, type, name, value, description, __VA_ARGS__)
#define FUSE_RELIGHT_OPTION_ENV(category, type, name, value, environmentVar, description)                          \
    FUSE_RELIGHT_OPTION_FULL(category, type, name, value, description, args.environment = environmentVar)
#define FUSE_RELIGHT_OPTION_FLAG(category, type, name, value, flagsValue, description)                             \
    FUSE_RELIGHT_OPTION_FULL(category, type, name, value, description,                                             \
                             args.flags = static_cast<std::uint32_t>(flagsValue))
#define FUSE_RELIGHT_OPTION_FLAG_ENV(category, type, name, value, flagsValue, environmentVar, description)          \
    FUSE_RELIGHT_OPTION_FULL(category, type, name, value, description,                                             \
                             args.flags = static_cast<std::uint32_t>(flagsValue);                                   \
                             args.environment = environmentVar)
