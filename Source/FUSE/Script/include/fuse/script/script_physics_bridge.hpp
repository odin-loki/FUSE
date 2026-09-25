#pragma once

#include <fuse/ecs/registry.hpp>
#include <fuse/physics/events/collision_events.hpp>
#include <fuse/physics/physics_manager.hpp>
#include <fuse/script/script_engine_api.hpp>

#include <vector>

namespace fuse::script {

class ScriptRuntime;

/// `Physics.*` script API on the FUSE `PhysicsManager` (non-owning references).
class PhysicsManagerScriptBackend final : public ScriptPhysicsBackend {
public:
    PhysicsManagerScriptBackend(physics::PhysicsManager& manager, ecs::Registry& registry)
        : m_manager(manager), m_registry(registry) {}

    bool ray_cast(const ecs::vec3& origin, const ecs::vec3& direction, f32 max_distance,
                  ScriptRayHit& out) override;
    bool apply_impulse(ecs::EntityID entity, const ecs::vec3& impulse) override;
    bool set_velocity(ecs::EntityID entity, const ecs::vec3& velocity) override;
    bool get_velocity(ecs::EntityID entity, ecs::vec3& out) override;

private:
    physics::PhysicsManager& m_manager;
    ecs::Registry& m_registry;
};

/// Forward one physics step's events to scripts: `Enter` -> `on_collision` and `Trigger` ->
/// `on_trigger_enter`, on both entities of the pair. `Stay`/`Exit` are not script callbacks.
/// Returns the number of callbacks dispatched.
usize dispatch_physics_events(const std::vector<physics::CollisionEvent>& events, ScriptRuntime& runtime);

} // namespace fuse::script
