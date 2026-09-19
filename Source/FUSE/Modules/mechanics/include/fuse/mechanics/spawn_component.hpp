#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/spawnComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::mechanics {

/// Spawn-point leaf (GMK SpawnComponent without SimObject/Con::).
class SpawnComponent : public Component {
public:
    SpawnComponent();
    explicit SpawnComponent(std::string name, std::string spawnId = {});

    const char* typeName() const override { return "SpawnComponent"; }

    const std::string& spawnId() const { return m_spawnId; }
    void setSpawnId(std::string spawnId) { m_spawnId = std::move(spawnId); }

    bool active() const { return m_active; }
    void setActive(bool active) { m_active = active; }

    u32 spawnCount() const { return m_spawnCount; }
    bool triggerSpawn();

private:
    std::string m_spawnId;
    bool m_active = true;
    u32 m_spawnCount = 0;
};

} // namespace fuse::mechanics
