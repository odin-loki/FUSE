#pragma once

#include <fuse/types.hpp>

namespace fuse {

struct LocalTransform2D {
    float x = 0.f;
    float y = 0.f;
};

struct WorldTransform2D {
    float x = 0.f;
    float y = 0.f;
};

struct LocalTransform3D {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

struct WorldTransform3D {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

class Object;
class SceneObject2D;

/// Returns nullptr when obj is not a SceneObject2D/SceneObject3D node (no RTTI).
const SceneObject2D* asSceneObject2D(const Object* obj);

} // namespace fuse
