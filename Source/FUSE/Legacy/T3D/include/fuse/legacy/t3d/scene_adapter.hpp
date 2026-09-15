#pragma once

#include <fuse/types.hpp>

namespace fuse {
class SceneObject3D;
}

namespace fuse::legacy::t3d {

struct LegacySceneObjectStub {
    u32 legacyId = 0;
    const char* name = nullptr;
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

bool importSceneObject(const LegacySceneObjectStub& legacy, SceneObject3D& out);
bool exportSceneObject(const SceneObject3D& src, LegacySceneObjectStub& out);

} // namespace fuse::legacy::t3d
