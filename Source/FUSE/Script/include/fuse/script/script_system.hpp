#pragma once

// ScriptSystem (MP-B7.3-SCRIPT-COMPONENT / UNI-U3-SCRIPT-1): drives ScriptRuntime behaviours from
// ecs::Script components.
//
// The ECS has no add/remove observers, so the system reconciles once per `update` (and on demand
// with `sync`): every enabled Script component with a module gets a ScriptRuntime instance, and an
// instance is detached (on_destroy) when its component is removed or disabled, its module path
// changes (then it is re-attached), or its entity is destroyed. On attach the component's exposed
// properties are written into the instance's `self` table and `lua_ref` is set; `started` becomes
// true after the first update (when ScriptRuntime runs on_start). `enterPlay` / `exitPlay` give the
// editor's play-in-editor its lifecycle: nothing is attached or ticked outside play, and `exitPlay`
// destroys every behaviour and clears the components' runtime fields.
//
// Steady state (no component added / removed / edited) `update` makes no heap allocations: the
// tracking table is flat, scratch lists keep their capacity, and ScriptRuntime::update is itself
// allocation-free (Lua memory comes from the VM's heap pool, ScriptVMDesc::heap_reserve_bytes).

#include <fuse/asset/asset_id.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/ecs/registry_serialiser.hpp>
#include <fuse/types.hpp>

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::ecs {
class Registry;
struct Script;
} // namespace fuse::ecs

namespace fuse::script {

class ScriptRuntime;

/// Maps a Script component's `module` asset id to a loadable module path (used when the
/// component's `script_path` is empty). Return false when the id is unknown.
using ScriptModuleResolver = bool (*)(void* user, asset::AssetId module, std::string& out_path);

/// A contact for script callbacks, independent of the physics module (the fuse_script_physics
/// bridge converts PhysicsManager events to these, see script_physics_bridge.hpp).
struct ScriptContactEvent {
    enum class Kind : u8 { Collision, TriggerEnter };
    Kind kind = Kind::Collision;
    ecs::EntityID a = ecs::EntityID::null();
    ecs::EntityID b = ecs::EntityID::null();
    ecs::vec3 point{};
};

struct ScriptSystemStats {
    u64 attaches = 0;
    u64 detaches = 0;
    u64 load_failures = 0;
    u64 syncs = 0;
};

class ScriptSystem {
public:
    ScriptSystem() = default;
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    /// Non-owning: `registry` and `runtime` (initialised, normally bound to the same registry)
    /// must outlive the system. `expected_entities` pre-sizes the tracking tables.
    bool init(ecs::Registry& registry, ScriptRuntime& runtime, usize expected_entities = 256);
    /// Leaves play (destroying every behaviour) and unbinds.
    void shutdown();
    [[nodiscard]] bool is_initialized() const { return m_registry != nullptr; }

    void set_module_resolver(ScriptModuleResolver resolver, void* user);

    /// Play-in-editor / game start: attach behaviours for every Script component (next update
    /// runs their on_start). Clears remembered load failures so edited scripts are retried.
    void enterPlay();
    /// Detach every behaviour (on_destroy for started ones) and reset `started` / `lua_ref`.
    void exitPlay();
    [[nodiscard]] bool playing() const { return m_playing; }

    /// One frame (no-op outside play): reconcile components, tick the runtime (on_start once,
    /// then on_update(dt)), then mark newly started components.
    void update(f32 dt);
    /// Reconcile components with instances without ticking (no-op outside play).
    void sync();

    /// Call after the registry's contents were replaced wholesale (scene load): every instance
    /// of the previous scene is destroyed, then the loaded components are attached.
    void on_scene_loaded();
    /// RegistrySerialiser::load into the bound registry + `on_scene_loaded`.
    ecs::RegistrySerialiseResult load_scene(const std::string& path);

    /// Detach the entity's behaviour first (on_destroy still sees a live entity), then destroy it.
    void destroy_entity(ecs::EntityID entity);

    /// Contact callbacks for behaviours this system attached (no-op outside play).
    bool dispatch_collision(ecs::EntityID entity, ecs::EntityID other, const ecs::vec3& point);
    bool dispatch_trigger_enter(ecs::EntityID entity, ecs::EntityID other);
    /// Both entities of each contact get their callback. Returns callbacks dispatched.
    usize dispatch_contacts(std::span<const ScriptContactEvent> events);

    [[nodiscard]] bool is_attached(ecs::EntityID entity) const;
    [[nodiscard]] usize attached_count() const { return m_attachedCount; }
    [[nodiscard]] const ScriptSystemStats& stats() const { return m_stats; }
    [[nodiscard]] const std::string& last_error() const { return m_lastError; }
    [[nodiscard]] ScriptRuntime* runtime() const { return m_runtime; }

private:
    struct Tracked {
        ecs::EntityID entity = ecs::EntityID::null();
        u64 identity = 0;        ///< hash of script_path + module id at attach time
        u32 stamp = 0;           ///< last sync that saw the component unchanged
        bool attached = false;   ///< false: the attach failed; retried when the identity changes
        bool pending_start = false;
    };

    static u64 key_of(ecs::EntityID entity) {
        return (static_cast<u64>(entity.generation) << 32) | static_cast<u64>(entity.index);
    }
    [[nodiscard]] const Tracked* find_tracked(ecs::EntityID entity) const;
    void attach_entity(ecs::EntityID entity);
    void release_tracked(usize index);
    void detach_all();
    bool resolve_module(const ecs::Script& script, std::string& out_path);

    ecs::Registry* m_registry = nullptr;
    ScriptRuntime* m_runtime = nullptr;
    ScriptModuleResolver m_resolver = nullptr;
    void* m_resolverUser = nullptr;
    bool m_playing = false;
    u32 m_stamp = 0;
    usize m_attachedCount = 0;
    usize m_pendingStarts = 0;
    std::vector<Tracked> m_tracked;
    std::unordered_map<u64, u32> m_index; ///< key_of(entity) -> m_tracked slot
    std::vector<ecs::EntityID> m_toAttach; ///< scratch (capacity kept)
    std::string m_modulePath;              ///< scratch (capacity kept)
    std::string m_lastError;
    ScriptSystemStats m_stats{};
};

} // namespace fuse::script
