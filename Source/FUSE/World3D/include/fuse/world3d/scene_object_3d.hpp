#pragma once

#include <fuse/types.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/scene_transform.hpp>

namespace fuse {

/// 3D scene node: the SceneObject2D TRS transform (translation + quaternion + scale, cached world
/// matrix composed along the parent chain; UNI-WP05-1) with depth and Euler accessors.
/// Render/collision backends are composed components — not base classes.
class SceneObject3D : public SceneObject2D {
public:
    SceneObject3D();
    explicit SceneObject3D(std::string name);

    const char* typeName() const override { return "SceneObject3D"; }
    SceneNodeKind sceneNodeKind() const override { return SceneNodeKind::Node3D; }

    float z() const { return localTranslation().z; }
    void setZ(float z);
    void setPosition3D(float x, float y, float z) { setLocalTranslation({x, y, z}); }
    using SceneObject2D::setPosition;

    /// Euler setters convert to the local quaternion (R = Rz(yaw) * Rx(pitch) * Ry(roll)); each keeps
    /// the other two angles. Getters return the last Euler values set (or decomposed from
    /// setLocalRotation).
    float yawDeg() const { return eulerYawDeg(); }
    void setYawDeg(float yawDeg) { setEulerDeg(yawDeg, eulerPitchDeg(), eulerRollDeg()); }

    float pitchDeg() const { return eulerPitchDeg(); }
    void setPitchDeg(float pitchDeg) { setEulerDeg(eulerYawDeg(), pitchDeg, eulerRollDeg()); }

    float rollDeg() const { return eulerRollDeg(); }
    void setRollDeg(float rollDeg) { setEulerDeg(eulerYawDeg(), eulerPitchDeg(), rollDeg); }

    LocalTransform3D localTransform3D() const;
    /// Decomposition of worldMatrix() (the matrix itself is in `.matrix`).
    WorldTransform3D worldTransform3D() const;
};

/// Returns nullptr when obj is not a SceneObject3D node (no RTTI, no string compare).
const SceneObject3D* asSceneObject3D(const Object* obj);
SceneObject3D* asSceneObject3D(Object* obj);

} // namespace fuse
