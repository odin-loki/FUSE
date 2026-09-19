#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse {
class SceneObject2D;
}

namespace fuse::legacy::t2d {

/// Stand-in for quarantined T2D SceneObject fields — no Engine pointers cross this boundary.
enum class LegacyPhysicsShape : u8 {
    None = 0,
    Circle = 1,
    Box = 2,
};

struct LegacySceneObjectStub {
    u32 legacyId = 0;
    std::string name;
    float x = 0.f;
    float y = 0.f;
    s32 layer = 0;
    u32 sortKey = 0;
    bool compositeSprite = false;
    bool physicsEnabled = false;
    s32 collisionLayer = 0;
    u32 collisionMask = 0xFFFFFFFFu;
    LegacyPhysicsShape physicsShape = LegacyPhysicsShape::None;
    float physicsRadius = 0.5f;
    float boxHalfWidth = 0.5f;
    float boxHalfHeight = 0.5f;
};

/// Import legacy stub fields into a greenfield node (game thread only).
bool importSceneObject(const LegacySceneObjectStub& legacy, SceneObject2D& out);

/// Export greenfield fields into a stub record for round-trip tests (game thread only).
bool exportSceneObject(const SceneObject2D& src, LegacySceneObjectStub& out);

} // namespace fuse::legacy::t2d
