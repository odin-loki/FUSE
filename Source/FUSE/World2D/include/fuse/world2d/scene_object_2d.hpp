#pragma once

#include <fuse/math/mat.hpp>
#include <fuse/math/quat.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>
#include <fuse/world2d/scene_transform.hpp>

namespace fuse {

class SceneHandleTable;

enum class PhysicsShape2D : u8 {
    None = 0,
    Circle = 1,
    Box = 2,
};

/// 2D scene node (WP-05) and the transform base of SceneObject3D.
///
/// Transform (UNI-WP05-1): local translation + unit quaternion + per-axis scale; the world matrix is
/// parentWorld * T * R * S, composed along the chain of scene ancestors and cached per node. A
/// setter marks the node dirty; descendants notice through the parent's world version stamp
/// (each cached matrix records which parent matrix version it was built from), so a change, a
/// reparent or a parent's move invalidates the whole subtree lazily without any per-change walk,
/// and an unchanged hierarchy costs one version compare per node per query.
///
/// Threading: game thread only (the cache is filled from const accessors). Workers read the world
/// matrices exported into the snapshot SoA and refer to nodes by Handle<Object> (SceneHandleTable).
/// Legacy T2D SceneObject adapters will wrap this type; do not inherit legacy GL types here.
class SceneObject2D : public Object {
public:
    SceneObject2D();
    explicit SceneObject2D(std::string name);
    ~SceneObject2D() override;

    const char* typeName() const override { return "SceneObject2D"; }
    SceneNodeKind sceneNodeKind() const override { return SceneNodeKind::Node2D; }

    // --- local transform -------------------------------------------------------------------------
    float x() const { return m_translation.x; }
    float y() const { return m_translation.y; }
    /// Local x / y (z is kept).
    void setPosition(float x, float y);

    const math::Vec3& localTranslation() const { return m_translation; }
    void setLocalTranslation(const math::Vec3& translation);
    const math::Quat& localRotation() const { return m_rotation; }
    /// Normalised on store; the Euler accessors are updated from it.
    void setLocalRotation(const math::Quat& rotation);
    const math::Vec3& localScale() const { return m_scale; }
    void setLocalScale(const math::Vec3& scale);

    /// 2D rotation about +Z in radians (the yaw).
    float rotation() const;
    void setRotation(float radians);
    /// 2D scale (z scale stays as is).
    void setScale(float sx, float sy);

    /// Euler setters/getters in degrees (see scene_transform.hpp for the axis convention). Setting one
    /// angle keeps the other two, so setYaw / setPitch / setRoll in any order give the same rotation.
    void setEulerDeg(float yawDeg, float pitchDeg, float rollDeg);
    float eulerYawDeg() const { return m_yawDeg; }
    float eulerPitchDeg() const { return m_pitchDeg; }
    float eulerRollDeg() const { return m_rollDeg; }

    /// T * R * S.
    math::Mat4 localMatrix() const;

    // --- world transform ---------------------------------------------------------------------------
    /// Cached parentWorld * localMatrix() (recomputed only when this node or an ancestor changed).
    const math::Mat4& worldMatrix() const;
    /// worldMatrix() for a caller that walks the tree top-down: `currentParent` must be this node's
    /// sceneParent() and its cache must already be current (skips the ancestor walk).
    const math::Mat4& worldMatrixUnder(const SceneObject2D* currentParent) const { return refreshWorld_(currentParent); }
    /// Stamp of the current cached world matrix (changes whenever the matrix is recomputed).
    u64 worldVersion() const;
    math::Vec3 worldTranslation() const;

    LocalTransform2D localTransform() const;
    WorldTransform2D worldTransform() const;

    /// Sets the local transform so the world translation / rotation become the given values (the
    /// local scale is kept). Used by physics write-back for nodes under a transformed parent.
    void setWorldPose(const math::Vec3& worldTranslation, const math::Quat& worldRotation);
    /// Reparents under `newParent` (nullptr unparents) and rewrites the local TRS so the world matrix
    /// is unchanged (exact unless the new parent chain adds shear).
    void reparentKeepWorld(Object* newParent);

    /// Nearest ancestor that is a scene node (non-scene Objects are skipped), or nullptr.
    const SceneObject2D* sceneParent() const;

    // --- 2D presentation / physics settings --------------------------------------------------------
    s32 layer() const { return m_layer; }
    void setLayer(s32 layer) { m_layer = layer; }

    u32 sortKey() const { return m_sortKey; }
    void setSortKey(u32 key) { m_sortKey = key; }

    bool physicsEnabled() const { return m_physicsEnabled; }
    void setPhysicsEnabled(bool enabled) { m_physicsEnabled = enabled; }

    PhysicsShape2D physicsShape() const { return m_physicsShape; }
    void setPhysicsShape(PhysicsShape2D shape) { m_physicsShape = shape; }

    s32 collisionLayer() const { return m_collisionLayer; }
    void setCollisionLayer(s32 layer) { m_collisionLayer = layer; }

    u32 collisionMask() const { return m_collisionMask; }
    void setCollisionMask(u32 mask) { m_collisionMask = mask; }

    float physicsRadius() const { return m_physicsRadius; }
    void setPhysicsRadius(float radius) { m_physicsRadius = radius; }

    float boxHalfWidth() const { return m_boxHalfWidth; }
    void setBoxHalfWidth(float halfWidth) { m_boxHalfWidth = halfWidth; }

    float boxHalfHeight() const { return m_boxHalfHeight; }
    void setBoxHalfHeight(float halfHeight) { m_boxHalfHeight = halfHeight; }

    /// Table this node is published in (nullptr when unpublished).
    const SceneHandleTable* handleTable() const { return m_handleTable; }

protected:
    void markTransformDirty_() { m_localDirty = true; }

private:
    friend class SceneHandleTable;

    /// Recomputes the cache from `parent` (whose own cache must be current).
    const math::Mat4& refreshWorld_(const SceneObject2D* parent) const;

    math::Vec3 m_translation{};
    math::Quat m_rotation{};
    math::Vec3 m_scale{1.f, 1.f, 1.f};
    float m_yawDeg = 0.f;
    float m_pitchDeg = 0.f;
    float m_rollDeg = 0.f;

    mutable math::Mat4 m_world = math::Mat4::identity();
    mutable u64 m_worldVersion = 0;
    mutable u64 m_cachedParentVersion = 0;
    mutable const SceneObject2D* m_cachedParent = nullptr;
    mutable bool m_localDirty = true;

    SceneHandleTable* m_handleTable = nullptr;

    s32 m_layer = 0;
    u32 m_sortKey = 0;
    bool m_physicsEnabled = false;
    PhysicsShape2D m_physicsShape = PhysicsShape2D::None;
    s32 m_collisionLayer = 0;
    u32 m_collisionMask = 0xFFFFFFFFu;
    float m_physicsRadius = 0.5f;
    float m_boxHalfWidth = 0.5f;
    float m_boxHalfHeight = 0.5f;
};

} // namespace fuse
