#pragma once

#include <fuse/scene/camera.hpp>
#include <fuse/scene/scene_snapshot.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::scene {

/// Minimal scene container — camera + entity table (B3.6/B3.9 stub).
class Scene {
public:
    explicit Scene(std::string name = "Untitled");

    const std::string& name() const { return m_name; }
    void setName(std::string name);

    Camera& camera() { return m_camera; }
    const Camera& camera() const { return m_camera; }

    u32 entityCount() const { return static_cast<u32>(m_entities.size()); }
    u32 objectCount() const { return entityCount(); }

    void addEntity(std::string entityName, SceneEntityTransform transform = {});
    void addObjectName(std::string objectName);

    const std::vector<SceneEntity>& entities() const { return m_entities; }
    const std::vector<std::string>& objectNames() const;
    void clearEntities();
    void clearObjects();

    SceneSnapshot captureSnapshot() const { return SceneSnapshot::capture(*this); }
    void applySnapshot(const SceneSnapshot& snapshot) { snapshot.apply(*this); }

private:
    void rebuildObjectNameCache_() const;

    std::string m_name;
    Camera m_camera;
    std::vector<SceneEntity> m_entities;
    mutable std::vector<std::string> m_objectNamesCache;
};

} // namespace fuse::scene
