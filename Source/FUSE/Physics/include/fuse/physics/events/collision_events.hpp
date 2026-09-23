#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/physics/math.hpp>

#include <functional>
#include <unordered_map>
#include <vector>

namespace fuse::physics {

enum class CollisionEventType : u8 {
    Enter,
    Stay,
    Exit,
    Trigger,
    /// A joint exceeded its break force / torque (entityB null: jointed to the world).
    /// contactPoint = world anchor on A, impulse = constraint force (N) at the break,
    /// jointIndex / jointGeneration = the broken `JointHandle`.
    JointBreak,
};

struct CollisionEvent {
    CollisionEventType type = CollisionEventType::Enter;
    fuse::ecs::EntityID entityA{};
    fuse::ecs::EntityID entityB{};
    vec3 contactPoint{};
    vec3 contactNormal{};
    f32 impulse = 0.f;
    u32 jointIndex = 0xFFFFFFFFu;
    u32 jointGeneration = 0;
};

using CollisionCallback = std::function<void(const CollisionEvent&)>;

/// B4.10 — Enter/Stay/Exit/Trigger event bus scaffold.
class CollisionEventSystem {
public:
    void registerCallback(fuse::ecs::EntityID entity, CollisionCallback callback);
    void unregisterCallback(fuse::ecs::EntityID entity);
    void dispatch(const std::vector<CollisionEvent>& events);

    u32 callbackCount() const { return static_cast<u32>(m_callbacks.size()); }
    u32 dispatchedCount() const { return m_dispatchedCount; }

private:
    std::unordered_map<u32, CollisionCallback> m_callbacks{};
    u32 m_dispatchedCount = 0;
};

} // namespace fuse::physics
