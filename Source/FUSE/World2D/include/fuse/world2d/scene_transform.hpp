#pragma once

// UNI-WP05-1: full TRS transforms for the greenfield scene graph (SceneObject2D / SceneObject3D).
//
// Every scene node stores a local translation, a unit quaternion and a per-axis scale. Its world
// matrix is parentWorld * T * R * S along the chain of scene ancestors (non-scene Objects in the
// chain are transparent), cached per node. The helpers below are the matrix math the cache, the
// snapshot export and the physics sync share.
//
// Axis convention (Torque / cinematics compatible): +Z is up. Yaw rotates about +Z, pitch about +X,
// roll about +Y, applied roll first, then pitch, then yaw: R = Rz(yaw) * Rx(pitch) * Ry(roll). A 2D
// node's rotation is its yaw (radians) and its scale is (sx, sy, 1).

#include <fuse/math/mat.hpp>
#include <fuse/math/quat.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse {

/// Local (parent-relative) transform of a 2D node.
struct LocalTransform2D {
    float x = 0.f;
    float y = 0.f;
    float rotation = 0.f; ///< radians about +Z
    float scaleX = 1.f;
    float scaleY = 1.f;
};

/// World transform of a 2D node, decomposed from its cached world matrix.
struct WorldTransform2D {
    float x = 0.f;
    float y = 0.f;
    float rotation = 0.f; ///< radians about +Z (atan2 of the world X axis)
    float scaleX = 1.f;   ///< length of the world X axis
    float scaleY = 1.f;   ///< length of the world Y axis
};

/// Local transform of a 3D node (Euler angles in degrees are the setter values; see the header note).
struct LocalTransform3D {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float yaw_deg = 0.f;
    float pitch_deg = 0.f;
    float roll_deg = 0.f;
    math::Quat rotation{};
    math::Vec3 scale{1.f, 1.f, 1.f};
};

/// World transform of a 3D node, decomposed from its cached world matrix (rotation / scale assume
/// the matrix has no shear; `matrix` is always exact).
struct WorldTransform3D {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float yaw_deg = 0.f; ///< heading of the world X axis about +Z
    math::Quat rotation{};
    math::Vec3 scale{1.f, 1.f, 1.f};
    math::Mat4 matrix = math::Mat4::identity();
};

class Object;
class SceneObject2D;

/// Returns nullptr when obj is not a SceneObject2D/SceneObject3D node (no RTTI).
const SceneObject2D* asSceneObject2D(const Object* obj);
SceneObject2D* asSceneObject2D(Object* obj);

namespace scene_math {

/// Quaternion from Euler degrees (R = Rz(yaw) * Rx(pitch) * Ry(roll)).
math::Quat quatFromEulerDeg(float yawDeg, float pitchDeg, float rollDeg);
/// Inverse of quatFromEulerDeg (pitch in [-90, 90]; gimbal lock puts the whole twist in yaw).
void eulerDegFromQuat(const math::Quat& q, float& yawDeg, float& pitchDeg, float& rollDeg);

/// T * R * S (column-major, column vectors).
math::Mat4 composeTRS(const math::Vec3& t, const math::Quat& r, const math::Vec3& s);
/// Full inverse of an affine matrix (general 3x3 block, cofactor inverse). Singular blocks return identity.
math::Mat4 inverseAffineGeneral(const math::Mat4& m);
/// Splits an affine matrix into T, R, S (scale = column lengths, negative x scale for a mirror).
/// Exact when the 3x3 block is rotation * diagonal scale (no shear).
void decomposeTRS(const math::Mat4& m, math::Vec3& t, math::Quat& r, math::Vec3& s);
/// Unit quaternion of an orthonormal 3x3 block given as columns.
math::Quat quatFromBasis(const math::Vec3& x, const math::Vec3& y, const math::Vec3& z);

/// Largest absolute element difference.
float maxAbsDiff(const math::Mat4& a, const math::Mat4& b);

} // namespace scene_math

} // namespace fuse
