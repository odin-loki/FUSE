#include <fuse/mechanics/spawn_component.hpp>

namespace fuse::mechanics {

SpawnComponent::SpawnComponent() : Component("SpawnComponent") {}

SpawnComponent::SpawnComponent(std::string name, std::string spawnId)
    : Component(std::move(name)), m_spawnId(std::move(spawnId)) {}

bool SpawnComponent::triggerSpawn() {
    if (!m_active || m_spawnId.empty()) {
        return false;
    }
    ++m_spawnCount;
    return true;
}

} // namespace fuse::mechanics
