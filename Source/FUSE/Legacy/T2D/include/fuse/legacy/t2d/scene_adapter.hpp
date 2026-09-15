#pragma once

#include <fuse/types.hpp>

namespace fuse {
class SceneObject2D;
}

namespace fuse::legacy::t2d {

/// Stand-in for quarantined T2D SceneObject fields — no Engine pointers cross this boundary.
struct LegacySceneObjectStub {
    u32 legacyId = 0;
    const char* name = nullptr;
    float x = 0.f;
    float y = 0.f;
    s32 layer = 0;
};

/// Stub import: copies stub fields into greenfield node (game thread only).
bool importSceneObject(const LegacySceneObjectStub& legacy, SceneObject2D& out);

/// Stub export: copies greenfield fields into stub record for round-trip tests.
bool exportSceneObject(const SceneObject2D& src, LegacySceneObjectStub& out);

} // namespace fuse::legacy::t2d
