#include <fuse/physics/events/collision_events.hpp>

namespace fuse::physics {

void CollisionEventSystem::registerCallback(fuse::ecs::EntityID entity, CollisionCallback callback) {
    if (!entity.valid() || !callback) {
        return;
    }
    m_callbacks[entity.index] = std::move(callback);
}

void CollisionEventSystem::unregisterCallback(fuse::ecs::EntityID entity) {
    if (!entity.valid()) {
        return;
    }
    m_callbacks.erase(entity.index);
}

void CollisionEventSystem::dispatch(const std::vector<CollisionEvent>& events) {
    for (const CollisionEvent& event : events) {
        if (event.entityA.valid()) {
            const auto it = m_callbacks.find(event.entityA.index);
            if (it != m_callbacks.end() && it->second) {
                it->second(event);
            }
        }
        if (event.entityB.valid()) {
            const auto it = m_callbacks.find(event.entityB.index);
            if (it != m_callbacks.end() && it->second) {
                it->second(event);
            }
        }
        ++m_dispatchedCount;
    }
}

} // namespace fuse::physics
