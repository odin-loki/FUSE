#include <fuse/legacy/t2d/scene_adapter.hpp>

#include <fuse/platform/thread.hpp>
#include <fuse/world2d/scene_object_2d.hpp>

namespace fuse::legacy::t2d {

bool importSceneObject(const LegacySceneObjectStub& legacy, SceneObject2D& out) {
    if (!fuse::platform::isMainThread()) {
        return false;
    }

    out.setLegacyId(legacy.legacyId);
    if (!legacy.name.empty()) {
        out.setName(legacy.name);
    }
    out.setPosition(legacy.x, legacy.y);
    out.setLayer(legacy.layer);
    out.setSortKey(legacy.sortKey);
    out.setPhysicsEnabled(legacy.physicsEnabled);
    out.setCollisionLayer(legacy.collisionLayer);
    out.setCollisionMask(legacy.collisionMask);
    out.setPhysicsRadius(legacy.physicsRadius);
    out.setBoxHalfWidth(legacy.boxHalfWidth);
    out.setBoxHalfHeight(legacy.boxHalfHeight);
    switch (legacy.physicsShape) {
    case LegacyPhysicsShape::Circle:
        out.setPhysicsShape(PhysicsShape2D::Circle);
        break;
    case LegacyPhysicsShape::Box:
        out.setPhysicsShape(PhysicsShape2D::Box);
        break;
    default:
        out.setPhysicsShape(PhysicsShape2D::None);
        break;
    }
    return true;
}

bool exportSceneObject(const SceneObject2D& src, LegacySceneObjectStub& out) {
    if (!fuse::platform::isMainThread()) {
        return false;
    }

    out.legacyId = src.legacyId();
    out.name = src.name();
    out.x = src.x();
    out.y = src.y();
    out.layer = src.layer();
    out.sortKey = src.sortKey();
    out.physicsEnabled = src.physicsEnabled();
    out.collisionLayer = src.collisionLayer();
    out.collisionMask = src.collisionMask();
    out.physicsRadius = src.physicsRadius();
    out.boxHalfWidth = src.boxHalfWidth();
    out.boxHalfHeight = src.boxHalfHeight();
    switch (src.physicsShape()) {
    case PhysicsShape2D::Circle:
        out.physicsShape = LegacyPhysicsShape::Circle;
        break;
    case PhysicsShape2D::Box:
        out.physicsShape = LegacyPhysicsShape::Box;
        break;
    default:
        out.physicsShape = LegacyPhysicsShape::None;
        break;
    }
    return true;
}

} // namespace fuse::legacy::t2d
