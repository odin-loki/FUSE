#pragma once

#include <fuse/ecs/math/vec.hpp>

namespace fuse::ecs {

/// Collision shape for the physics bridge (B4.9). Shape ids mirror fuse::physics::CollisionShapeType.
struct Collider {
    static constexpr const char* component_name = "Collider";

    /// No shape-pool reference (Collider::shape_ref).
    static constexpr u32 kNoShapeRef = 0xFFFFFFFFu;

    enum Shape : u32 {
        Sphere = 0,
        Box = 1,
        Capsule = 2,
        /// Convex hull from the shared shape pool (a cooked `.fusecol` hull or a runtime-built one).
        ConvexHull = 3,
        /// Signed distance field (analytic / SVO sampler) from the shape pool.
        SdfMesh = 4,
        /// Voxel volume from the shape pool; a destructible registered with the PhysicsManager uses
        /// its own volume automatically.
        Voxel = 5,
        Plane = 6,
        /// Static triangle mesh with a BVH (a cooked `.fusecol` mesh); static bodies only.
        TriMesh = 7,
    };

    u32 shape = Sphere;
    /// Sphere: x = radius. Box: half extents. Capsule: x = radius, y = half height. Plane: normal.
    /// Pooled shapes (hull / mesh / SDF / voxel): filled from the shape's bounds by the physics bridge.
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
    /// ConvexHull / TriMesh: the cooked collision asset (`.fusecol`, loaded with
    /// fuse::physics::loadCollisionAsset under this id) and which of its hulls / meshes to use
    /// (`shape_piece`). 0 = no asset.
    u64 shape_asset = 0;
    u32 shape_piece = 0;
    /// Direct shape-pool reference for runtime-registered shapes (wins over shape_asset).
    u32 shape_ref = kNoShapeRef;
};

} // namespace fuse::ecs
