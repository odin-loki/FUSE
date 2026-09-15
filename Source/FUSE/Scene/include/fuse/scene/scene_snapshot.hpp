#pragma once

#include <fuse/scene/camera.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::scene {

/// Per-entity transform payload stored alongside the legacy object-name table (B3.7 deepen).
struct SceneEntityTransform {
    float positionX = 0.f;
    float positionY = 0.f;
    float positionZ = 0.f;
    float rotationX = 0.f;
    float rotationY = 0.f;
    float rotationZ = 0.f;
    float rotationW = 1.f;
    float scaleX = 1.f;
    float scaleY = 1.f;
    float scaleZ = 1.f;
};

struct SceneEntity {
    std::string name;
    SceneEntityTransform transform{};
};

/// In-memory scene snapshot for play-mode restore and serialiser round-trip stubs.
struct SceneSnapshot {
    std::string name;
    Camera camera{};
    std::vector<SceneEntity> entities;

    static SceneSnapshot capture(const class Scene& scene);
    void apply(class Scene& scene) const;
};

} // namespace fuse::scene
