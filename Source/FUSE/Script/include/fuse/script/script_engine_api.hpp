#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::ecs {
class Registry;
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

/// Engine services exposed to scripts. Non-owning: both must outlive script execution.
struct ScriptEngineBindings {
    ecs::Registry* registry = nullptr;
    ScriptPhysicsBackend* physics = nullptr;
};

/// Register the public script API on the VM's Lua state:
///
///   Entity.create([name]) -> id          Entity.destroy(id)          Entity.alive(id) -> bool
///   Entity.get_position(id) -> {x,y,z}   Entity.set_position(id, {x,y,z}) -> bool
///   Entity.get_rotation(id) -> {x,y,z,w} Entity.set_rotation_euler(id, {x,y,z} degrees) -> bool
///   Physics.ray_cast(origin, dir, max) -> {entity, point, normal, distance} | nil
///   Physics.apply_impulse(id, v)  Physics.set_velocity(id, v)  Physics.get_velocity(id) -> v|nil
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
