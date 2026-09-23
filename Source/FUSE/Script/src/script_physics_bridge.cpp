#include <fuse/script/script_physics_bridge.hpp>

#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/script/script_runtime.hpp>

namespace fuse::script {

namespace {

physics::vec3 to_physics(const ecs::vec3& v) { return {v.x, v.y, v.z}; }

ecs::vec3 to_ecs(const physics::vec3& v) { return {v.x, v.y, v.z, 0.f}; }

constexpr u32 kNoBody = ~0u;

} // namespace

bool PhysicsManagerScriptBackend::ray_cast(const ecs::vec3& origin, const ecs::vec3& direction,
                                           f32 max_distance, ScriptRayHit& out) {
    const physics::vec3 o = to_physics(origin);
    const physics::vec3 d = to_physics(direction);
    if (d.length() < 1e-6f) {
        return false;
    }
    // The manager's shapes reflect the last physics step: an entity destroyed since (e.g. by a
    // script in on_collision) keeps its body until the next step. Never hand scripts that dead
    // handle — filter hits by the registry.
    ecs::EntityID hit = ecs::EntityID::null();
    physics::vec3 normal{};
    f32 t = 0.f;
    if (!m_manager.rayCast(o, d, max_distance, hit, normal, t, &m_registry)) {
        return false;
    }
    out.entity = hit;
    out.distance = t;
    out.normal = to_ecs(normal);
    out.point = to_ecs(o + d.normalized() * t);
    return true;
}

bool PhysicsManagerScriptBackend::apply_impulse(ecs::EntityID entity, const ecs::vec3& impulse) {
    if (!m_registry.alive(entity) || m_manager.bodyIndex(entity) == kNoBody) {
        return false;
    }
    m_manager.applyImpulse(entity, to_physics(impulse));
    return true;
}

bool PhysicsManagerScriptBackend::set_velocity(ecs::EntityID entity, const ecs::vec3& velocity) {
    if (!m_registry.alive(entity)) {
        return false;
    }
    if (m_manager.bodyIndex(entity) != kNoBody) {
        m_manager.setVelocity(entity, to_physics(velocity));
        return true;
    }
    // Not simulated yet: the next step picks the component velocity up.
    if (ecs::RigidBody* rb = m_registry.get<ecs::RigidBody>(entity)) {
        rb->velocity = {velocity.x, velocity.y, velocity.z, 0.f};
        return true;
    }
    return false;
}

bool PhysicsManagerScriptBackend::get_velocity(ecs::EntityID entity, ecs::vec3& out) {
    if (!m_registry.alive(entity)) {
        return false;
    }
    const u32 body = m_manager.bodyIndex(entity);
    if (body != kNoBody) {
        out = to_ecs(m_manager.bodies().linearVelocities[body]);
        return true;
    }
    if (const ecs::RigidBody* rb = m_registry.get<ecs::RigidBody>(entity)) {
        out = rb->velocity;
        return true;
    }
    return false;
}

usize dispatch_physics_events(const std::vector<physics::CollisionEvent>& events, ScriptRuntime& runtime) {
    usize dispatched = 0;
    for (const physics::CollisionEvent& event : events) {
        const ecs::vec3 point = to_ecs(event.contactPoint);
        switch (event.type) {
        case physics::CollisionEventType::Enter:
            if (runtime.is_attached(event.entityA)) {
                runtime.dispatch_collision(event.entityA, event.entityB, point);
                ++dispatched;
            }
            if (runtime.is_attached(event.entityB)) {
                runtime.dispatch_collision(event.entityB, event.entityA, point);
                ++dispatched;
            }
            break;
        case physics::CollisionEventType::Trigger:
            if (runtime.is_attached(event.entityA)) {
                runtime.dispatch_trigger_enter(event.entityA, event.entityB);
                ++dispatched;
            }
            if (runtime.is_attached(event.entityB)) {
                runtime.dispatch_trigger_enter(event.entityB, event.entityA);
                ++dispatched;
            }
            break;
        default:
            break;
        }
    }
    return dispatched;
}

} // namespace fuse::script
