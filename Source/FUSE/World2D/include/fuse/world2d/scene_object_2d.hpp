#pragma once

#include <fuse/object.hpp>
#include <fuse/types.hpp>
#include <fuse/world2d/transform_stubs.hpp>

namespace fuse {

enum class PhysicsShape2D : u8 {
    None = 0,
    Circle = 1,
    Box = 2,
};

/// 2D scene node — xy transform, layer, sort key (WP-05 start).
/// Legacy T2D SceneObject adapters will wrap this type; do not inherit legacy GL types here.
class SceneObject2D : public Object {
public:
    SceneObject2D();
    explicit SceneObject2D(std::string name);

    const char* typeName() const override { return "SceneObject2D"; }

    float x() const { return m_x; }
    float y() const { return m_y; }
    void setPosition(float x, float y);

    LocalTransform2D localTransform() const;
    WorldTransform2D worldTransform() const;

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

private:
    float m_x = 0.f;
    float m_y = 0.f;
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
