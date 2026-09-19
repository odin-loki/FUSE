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

    float yawDeg() const { return m_yawDeg; }
    void setYawDeg(float yawDeg) { m_yawDeg = yawDeg; }

    float pitchDeg() const { return m_pitchDeg; }
    void setPitchDeg(float pitchDeg) { m_pitchDeg = pitchDeg; }

    float rollDeg() const { return m_rollDeg; }
    void setRollDeg(float rollDeg) { m_rollDeg = rollDeg; }

    LocalTransform3D localTransform3D() const;
    WorldTransform3D worldTransform3D() const;

private:
    float m_z = 0.f;
    float m_yawDeg = 0.f;
    float m_pitchDeg = 0.f;
    float m_rollDeg = 0.f;
};

} // namespace fuse
