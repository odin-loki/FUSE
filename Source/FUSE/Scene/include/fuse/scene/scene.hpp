#pragma once

#include <fuse/scene/camera.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::scene {

/// Minimal scene container — camera + named root (B3.6/B3.9 stub).
class Scene {
public:
    explicit Scene(std::string name = "Untitled");

    const std::string& name() const { return m_name; }
    void setName(std::string name);

    Camera& camera() { return m_camera; }
    const Camera& camera() const { return m_camera; }

    u32 objectCount() const { return static_cast<u32>(m_objectNames.size()); }
    void addObjectName(std::string objectName);
    const std::vector<std::string>& objectNames() const { return m_objectNames; }
    void clearObjects();

private:
    std::string m_name;
    Camera m_camera;
    std::vector<std::string> m_objectNames;
};

} // namespace fuse::scene
