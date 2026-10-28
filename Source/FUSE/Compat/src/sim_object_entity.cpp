#include <fuse/compat/sim_object_entity.hpp>

namespace fuse::compat {

void SimObjectEntityTable::bind(fuse::u32 simObjectId, fuse::ecs::EntityID entity) {
    if (!entity.valid()) {
        unbind(simObjectId);
        return;
    }

    const auto simIt = m_bySim.find(simObjectId);
    if (simIt != m_bySim.end() && simIt->second == entity) {
        return;
    }
    if (simIt != m_bySim.end()) {
        m_byEntity.erase(simIt->second);
        m_bySim.erase(simIt);
    }

    const auto entityIt = m_byEntity.find(entity);
    if (entityIt != m_byEntity.end()) {
        m_bySim.erase(entityIt->second);
        m_byEntity.erase(entityIt);
    }

    m_bySim.emplace(simObjectId, entity);
    m_byEntity.emplace(entity, simObjectId);
}

fuse::ecs::EntityID SimObjectEntityTable::find(fuse::u32 simObjectId) const {
    const auto it = m_bySim.find(simObjectId);
    if (it == m_bySim.end()) {
        return fuse::ecs::EntityID::null();
    }
    return it->second;
}

fuse::ecs::EntityID SimObjectEntityTable::entityFor(fuse::u32 simObjectId) const {
    return find(simObjectId);
}

std::optional<fuse::u32> SimObjectEntityTable::simObjectFor(fuse::ecs::EntityID entity) const {
    const auto it = m_byEntity.find(entity);
    if (it == m_byEntity.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool SimObjectEntityTable::unbind(fuse::u32 simObjectId) {
    const auto it = m_bySim.find(simObjectId);
    if (it == m_bySim.end()) {
        return false;
    }
    m_byEntity.erase(it->second);
    m_bySim.erase(it);
    return true;
}

bool SimObjectEntityTable::unbindEntity(fuse::ecs::EntityID entity) {
    const auto it = m_byEntity.find(entity);
    if (it == m_byEntity.end()) {
        return false;
    }
    m_bySim.erase(it->second);
    m_byEntity.erase(it);
    return true;
}

void SimObjectEntityTable::clear() {
    m_bySim.clear();
    m_byEntity.clear();
}

} // namespace fuse::compat
