#pragma once

// MP-B7.3-SCRIPT-COMPONENT / UNI-U3-SCRIPT-1: gameplay script attached to an entity.
//
// A plain, trivially copyable component (the registry stores columns as bytes and the scene
// serialiser copies them), so the module path and the exposed-property table live in fixed-size
// inline buffers. `fuse::script::ScriptSystem` owns the runtime side: it attaches a ScriptRuntime
// instance when the component appears, pushes `properties` into the instance's `self` table, calls
// on_start / on_update / on_destroy, and writes `started` / `lua_ref` back. Those two fields are
// runtime state only: RegistrySerialiser stores them cleared, so a loaded scene re-attaches its
// behaviours from scratch.

#include <fuse/asset/asset_id.hpp>
#include <fuse/types.hpp>

#include <cstring>
#include <string_view>

namespace fuse::ecs {

namespace detail {
/// Length of a NUL-terminated string stored in a fixed buffer of `capacity` bytes.
[[nodiscard]] inline usize bounded_strlen(const char* text, usize capacity) {
    usize n = 0;
    while (n < capacity && text[n] != '\0') {
        ++n;
    }
    return n;
}
} // namespace detail

/// One exposed script property (an editor-tunable value copied into `self[key]` on attach).
struct ScriptProperty {
    enum class Type : u8 { None = 0, Number = 1, Bool = 2, String = 3, Vec3 = 4 };

    static constexpr usize kKeyCapacity = 32;   ///< bytes incl. the terminating NUL
    static constexpr usize kTextCapacity = 64;  ///< bytes incl. the terminating NUL

    char key[kKeyCapacity] = {};
    Type type = Type::None;
    bool boolean = false;
    f64 number = 0.0;
    f32 vec[3] = {0.f, 0.f, 0.f};
    char text[kTextCapacity] = {};

    [[nodiscard]] std::string_view name() const { return {key, detail::bounded_strlen(key, kKeyCapacity)}; }
    [[nodiscard]] std::string_view string_value() const { return {text, detail::bounded_strlen(text, kTextCapacity)}; }
};

struct Script {
    static constexpr const char* component_name = "Script";

    static constexpr usize kPathCapacity = 256; ///< bytes incl. the terminating NUL
    static constexpr usize kMaxProperties = 8;
    /// `lua_ref` value while no instance is attached (Lua's LUA_NOREF).
    static constexpr i32 kNoRef = -2;

    /// Cooked script asset (fuse/asset/asset_id.hpp). Used when `script_path` is empty; the
    /// ScriptSystem's module resolver maps it to a loadable path.
    asset::AssetId module{};
    /// Module path (a `.lua` source, a cooked `.fusescript`, or the name of a module registered
    /// with ScriptRuntime::load_module_source). NUL-terminated.
    char script_path[kPathCapacity] = {};
    bool enabled = true;
    /// Runtime: on_start has run for the current instance (never serialised as true).
    bool started = false;
    /// Runtime: Lua registry reference of the instance's `self` table (kNoRef when detached).
    i32 lua_ref = kNoRef;
    u32 property_count = 0;
    ScriptProperty properties[kMaxProperties] = {};

    // ---- helpers (all bounded; overlong strings are rejected, never truncated) ---------------

    [[nodiscard]] std::string_view path() const { return {script_path, detail::bounded_strlen(script_path, kPathCapacity)}; }

    bool set_path(std::string_view value) {
        if (value.size() >= kPathCapacity) {
            return false;
        }
        std::memset(script_path, 0, sizeof(script_path));
        std::memcpy(script_path, value.data(), value.size());
        return true;
    }

    [[nodiscard]] const ScriptProperty* find_property(std::string_view key) const {
        const u32 count = property_count < kMaxProperties ? property_count : static_cast<u32>(kMaxProperties);
        for (u32 i = 0; i < count; ++i) {
            if (properties[i].name() == key) {
                return &properties[i];
            }
        }
        return nullptr;
    }

    bool set_number(std::string_view key, f64 value) {
        ScriptProperty* slot = slot_for(key);
        if (slot == nullptr) {
            return false;
        }
        slot->type = ScriptProperty::Type::Number;
        slot->number = value;
        return true;
    }

    bool set_bool(std::string_view key, bool value) {
        ScriptProperty* slot = slot_for(key);
        if (slot == nullptr) {
            return false;
        }
        slot->type = ScriptProperty::Type::Bool;
        slot->boolean = value;
        return true;
    }

    bool set_string(std::string_view key, std::string_view value) {
        if (value.size() >= ScriptProperty::kTextCapacity) {
            return false;
        }
        ScriptProperty* slot = slot_for(key);
        if (slot == nullptr) {
            return false;
        }
        slot->type = ScriptProperty::Type::String;
        std::memset(slot->text, 0, sizeof(slot->text));
        std::memcpy(slot->text, value.data(), value.size());
        return true;
    }

    bool set_vec3(std::string_view key, f32 x, f32 y, f32 z) {
        ScriptProperty* slot = slot_for(key);
        if (slot == nullptr) {
            return false;
        }
        slot->type = ScriptProperty::Type::Vec3;
        slot->vec[0] = x;
        slot->vec[1] = y;
        slot->vec[2] = z;
        return true;
    }

private:
    /// Existing property named `key`, else a fresh slot (nullptr when the key is invalid or full).
    ScriptProperty* slot_for(std::string_view key) {
        if (key.empty() || key.size() >= ScriptProperty::kKeyCapacity) {
            return nullptr;
        }
        if (const ScriptProperty* existing = find_property(key)) {
            return const_cast<ScriptProperty*>(existing);
        }
        if (property_count >= kMaxProperties) {
            return nullptr;
        }
        ScriptProperty& slot = properties[property_count++];
        slot = ScriptProperty{};
        std::memcpy(slot.key, key.data(), key.size());
        return &slot;
    }
};

} // namespace fuse::ecs
