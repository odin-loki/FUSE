#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/platform/input.hpp>
#include <fuse/types.hpp>

#include <string_view>
#include <vector>

namespace fuse::ecs {
class Registry;
}

namespace fuse::config {
class CVarRegistry;
}

namespace fuse::script {

class ScriptVM;

struct ScriptRayHit {
    ecs::EntityID entity = ecs::EntityID::null();
    ecs::vec3 point{};
    ecs::vec3 normal{};
    f32 distance = 0.f;
};

/// Physics services the `Physics.*` script table calls into. Implemented on the FUSE physics
/// module by `fuse_script_physics` (`PhysicsManagerScriptBackend`); fuse_script itself stays
/// independent of fuse_physics.
class ScriptPhysicsBackend {
public:
    virtual ~ScriptPhysicsBackend() = default;

    /// Nearest hit along `direction` (need not be normalized) within `max_distance`.
    virtual bool ray_cast(const ecs::vec3& origin, const ecs::vec3& direction, f32 max_distance,
                          ScriptRayHit& out) = 0;
    virtual bool apply_impulse(ecs::EntityID entity, const ecs::vec3& impulse) = 0;
    virtual bool set_velocity(ecs::EntityID entity, const ecs::vec3& velocity) = 0;
    virtual bool get_velocity(ecs::EntityID entity, ecs::vec3& out) = 0;
};

/// Input services behind the `Input.*` table (MP-B7.3-LUA-API / UNI-INPUT-1). The ready-made
/// implementation over InputState + ActionMap + GamepadState is `InputScriptBackend`
/// (script_engine_backends.hpp).
class ScriptInputBackend {
public:
    virtual ~ScriptInputBackend() = default;
    [[nodiscard]] virtual bool key_pressed(platform::Key key) const = 0;
    [[nodiscard]] virtual bool key_held(platform::Key key) const = 0;
    [[nodiscard]] virtual bool key_released(platform::Key key) const = 0;
    [[nodiscard]] virtual bool mouse_pressed(platform::MouseButton button) const = 0;
    [[nodiscard]] virtual bool mouse_held(platform::MouseButton button) const = 0;
    [[nodiscard]] virtual bool mouse_released(platform::MouseButton button) const = 0;
    virtual void mouse_delta(f32& dx, f32& dy) const = 0;
    virtual void mouse_position(f32& x, f32& y) const = 0;
    /// Action-map queries; false from `has_action` makes the Lua call raise (typo protection).
    [[nodiscard]] virtual bool has_action(std::string_view action) const = 0;
    [[nodiscard]] virtual bool action_pressed(std::string_view action) const = 0;
    [[nodiscard]] virtual bool action_held(std::string_view action) const = 0;
    [[nodiscard]] virtual bool action_released(std::string_view action) const = 0;
    [[nodiscard]] virtual f32 action_axis(std::string_view action) const = 0;
    [[nodiscard]] virtual bool gamepad_connected(u32 pad) const = 0;
};

/// Audio services behind the `Audio.*` table. Implemented on fuse::audio::AudioEngine by
/// `fuse_script_audio` (`AudioEngineScriptBackend`, script_audio_bridge.hpp). Clip handles are
/// opaque non-zero numbers (0 = load failed).
class ScriptAudioBackend {
public:
    virtual ~ScriptAudioBackend() = default;
    virtual u64 load(const char* path) = 0;
    virtual bool play_at(u64 clip, const ecs::vec3& position, f32 volume, f32 pitch) = 0;
    virtual bool play_2d(u64 clip, f32 volume) = 0;
};

/// Scene services behind the `Scene.*` table (and entity naming for `Entity.create(name)`).
/// `RegistrySceneBackend` (script_engine_backends.hpp) implements it over a Registry and an
/// optional spatial BVH (e.g. SceneManager::spatialBvh()).
class ScriptSceneBackend {
public:
    virtual ~ScriptSceneBackend() = default;
    /// Live entity named `name`, or EntityID::null().
    virtual ecs::EntityID find_entity(std::string_view name) = 0;
    /// Entities whose bounds intersect the sphere, appended to `out` in ascending index order.
    virtual usize query_sphere(const ecs::vec3& center, f32 radius, std::vector<ecs::EntityID>& out) = 0;
    /// Make `camera` (an entity with a Camera component) the only active camera.
    virtual bool set_active_camera(ecs::EntityID camera) = 0;
    [[nodiscard]] virtual ecs::EntityID active_camera() const = 0;
    /// Called by `Entity.create(name)` for a non-empty name.
    virtual void on_entity_created(ecs::EntityID entity, std::string_view name) {
        (void)entity;
        (void)name;
    }
};

/// Engine services exposed to scripts. Non-owning: each must outlive script execution. A table
/// whose service is null still exists; its functions raise a Lua error when called.
struct ScriptEngineBindings {
    ecs::Registry* registry = nullptr;
    ScriptPhysicsBackend* physics = nullptr;
    ScriptInputBackend* input = nullptr;
    ScriptAudioBackend* audio = nullptr;
    ScriptSceneBackend* scene = nullptr;
    /// `CVar.get` / `CVar.set` (runtime precedence). Null: the CVar table raises.
    config::CVarRegistry* cvars = nullptr;
};

/// Register the public script API on the VM's Lua state:
///
///   Entity.create([name]) -> id          Entity.destroy(id)          Entity.alive(id) -> bool
///   Entity.get_position(id) -> {x,y,z}   Entity.set_position(id, {x,y,z}) -> bool
///   Entity.get_rotation(id) -> {x,y,z,w} Entity.set_rotation_euler(id, {x,y,z} degrees) -> bool
///   Physics.ray_cast(origin, dir, max) -> {entity, point, normal, distance} | nil
///   Physics.apply_impulse(id, v)  Physics.set_velocity(id, v)  Physics.get_velocity(id) -> v|nil
///
///   Input.key_pressed(key) / key_held(key) / key_released(key) -> bool   (key: "Space", "W", "F1"...)
///   Input.mouse_pressed(btn) / mouse_held(btn) / mouse_released(btn) -> bool ("Left", "Right"...)
///   Input.mouse_delta() -> dx, dy          Input.mouse_position() -> x, y
///   Input.pressed(action) / held(action) / released(action) -> bool   Input.axis(action) -> number
///   Input.gamepad_connected(pad) -> bool   (pad 0..3)
///   Audio.load(path) -> clip|nil           Audio.play_at(clip, {x,y,z}[, volume[, pitch]]) -> bool
///   Audio.play_2d(clip[, volume]) -> bool
///   Scene.find_entity(name) -> id|nil      Scene.query_sphere({x,y,z}, radius) -> {id, ...}
///   Scene.set_active_camera(id) -> bool    Scene.active_camera() -> id|nil
///   SDF.set_radius(id, r) -> bool          SDF.set_alpha(id, a) -> bool (clamped to 0..1)
///   SDF.set_primitive(id, "Sphere"|"Box"|"Capsule"|"Torus"|"Cylinder"|"Custom") -> bool (params
///       re-derived from the current radius so the object keeps its size)
///   SDF.get(id) -> {primitive, radius, alpha, params={x,y,z}} | nil
///   CVar.get(name) -> bool|number|string|nil   CVar.set(name, value) -> true | false, reason
///
/// Any `id` argument may also be a behaviour's `self` table (its `entity` field is used), e.g.
/// `Entity.destroy(self)` from `on_collision`.
///
/// Scripts see only these tables — never registry or solver internals. Calls with a missing
/// service or malformed arguments raise a Lua error (caught by the VM's protected calls).
/// Returns false when the VM has no Lua backend.
bool bind_engine_api(ScriptVM& vm, const ScriptEngineBindings& bindings);

/// Entity handles cross into Lua as one number: `generation * 2^32 + index` (exact in a double
/// while generation < 2^21).
[[nodiscard]] f64 encode_entity_id(ecs::EntityID id);
[[nodiscard]] ecs::EntityID decode_entity_id(f64 value);

/// Yaw-pitch-roll (degrees): q = q_y(y) * q_x(x) * q_z(z).
[[nodiscard]] ecs::quat quat_from_euler_degrees(const ecs::vec3& euler_degrees);

} // namespace fuse::script
