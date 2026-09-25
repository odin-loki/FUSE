#pragma once

#include <fuse/ecs/math/vec.hpp>

namespace fuse::ecs {

/// Collision shape for the physics bridge (B4.9). Shape ids mirror fuse::physics::CollisionShapeType.
struct Collider {
    static constexpr const char* component_name = "Collider";

    enum Shape : u32 {
        Sphere = 0,
        Box = 1,
        Capsule = 2,
        Plane = 6,
    };

    u32 shape = Sphere;
    /// Sphere: x = radius. Box: half extents. Capsule: x = radius, y = half height. Plane: normal.
    vec3 params = {0.5f, 0.f, 0.f, 0.f};
    /// Plane: distance along the normal.
    f32 scalar = 0.f;
    f32 friction_static = 0.5f;
    f32 friction_dynamic = 0.3f;
    u32 layer = 1u;
    u32 mask = 0xFFFFFFFFu;
    /// Overlaps raise trigger events but are not resolved.
    bool is_trigger = false;
    /// Swept (continuous) collision for fast bodies.
    bool ccd = false;
};

} // namespace fuse::ecs
