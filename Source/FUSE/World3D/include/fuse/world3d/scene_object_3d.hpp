#pragma once

#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/types.hpp>

namespace fuse {

/// 3D scene node extending the 2D base with depth/orientation hooks (WP-05 start).
/// Render/collision backends are composed components — not base classes.
class SceneObject3D : public SceneObject2D {
public:
    SceneObject3D();
    explicit SceneObject3D(std::string name);

    const char* typeName() const override { return "SceneObject3D"; }

    float z() const { return m_z; }
    void setZ(float z) { m_z = z; }

private:
    float m_z = 0.f;
};

} // namespace fuse
