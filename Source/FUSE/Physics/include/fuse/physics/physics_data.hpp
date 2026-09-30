#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

enum class CollisionShapeType : u32 {
    Sphere = 0,
    Box = 1,
    Capsule = 2,
    ConvexHull = 3,
    SdfMesh = 4,
    Voxel = 5,
    Plane = 6,
    /// Static triangle mesh with a per-mesh BVH (shapes/tri_mesh.hpp).
    TriMesh = 7
};

/// CollisionShapeSoA::shapeRefs value of a shape with no pooled geometry (the primitives).
constexpr u32 kNoShapeRef = 0xFFFFFFFFu;

/// Hull, triangle mesh, SDF and voxel shapes keep their geometry in the shared shape pool
/// (shapes/shape_pool.hpp); their params hold the origin-centred bounding half extents.
[[nodiscard]] inline bool isPooledShape(CollisionShapeType type) {
    return type == CollisionShapeType::ConvexHull || type == CollisionShapeType::TriMesh ||
           type == CollisionShapeType::SdfMesh || type == CollisionShapeType::Voxel;
}

enum RigidBodyFlags : u32 {
    RB_STATIC = 1u << 0,
    RB_SLEEPING = 1u << 1,
    RB_TRIGGER = 1u << 2,
    RB_KINEMATIC = 1u << 3,
    RB_NO_GRAVITY = 1u << 4,
    RB_CCD = 1u << 5,
    /// Translation only: the solver gives the body zero inverse inertia (orientation never changes).
    RB_FIXED_ROTATION = 1u << 6
};

/// Torque2D-style layer/mask collision filter (bitmask layers).
[[nodiscard]] inline bool collisionLayersCollide(u32 layerA, u32 maskA, u32 layerB, u32 maskB) {
    return (layerA & maskB) != 0u && (layerB & maskA) != 0u;
}

/// Host-side SoA rigid body state (B4.1). GPU residency arrives in later B4 milestones.
struct RigidBodySoA {
    std::vector<vec3> positions;
    std::vector<quat> orientations;
    std::vector<vec3> linearVelocities;
    std::vector<vec3> angularVelocities;

    std::vector<f32> invMasses;
    std::vector<f32> restitutions;
    std::vector<f32> frictionStatic;
    std::vector<f32> frictionDynamic;
    std::vector<u32> flags;
    std::vector<u32> collisionLayers;
    std::vector<u32> collisionMasks;

    std::vector<vec3> predictedPositions;
    std::vector<quat> predictedOrientations;

    // B4.4 additive — external forces and sleep tracking for PBD solver.
    std::vector<vec3> forces;
    std::vector<vec3> torques;
    std::vector<f32> sleepTimers;

    u32 count() const { return static_cast<u32>(positions.size()); }
    u32 capacity() const { return static_cast<u32>(positions.capacity()); }

    void reserve(u32 bodyCapacity);
    void clear();
    u32 addBody(vec3 position, f32 invMass, u32 bodyFlags = 0, u32 collisionLayer = 1u,
                u32 collisionMask = 0xFFFFFFFFu);
    /// Moves the last body into `index` and shrinks by one (O(1); the last body's index changes).
    void removeBodySwap(u32 index);
};

/// Collision shape descriptors stored in SoA layout (B4.1).
struct CollisionShapeSoA {
    std::vector<u32> types;
    std::vector<vec3> params;
    std::vector<f32> scalars;
    std::vector<u32> bodyIndices;
    /// Shape-pool reference of pooled shapes (hull / mesh / SDF / voxel), kNoShapeRef otherwise.
    std::vector<u32> shapeRefs;

    u32 count() const { return static_cast<u32>(types.size()); }
    /// Pool reference of `shapeIndex` (kNoShapeRef when out of range or not pooled).
    u32 shapeRef(u32 shapeIndex) const { return shapeIndex < shapeRefs.size() ? shapeRefs[shapeIndex] : kNoShapeRef; }

    void clear();
    u32 addShape(CollisionShapeType type, u32 bodyIndex, vec3 shapeParams, f32 scalarParam = 0.f);
    /// Pooled shape: `ref` from the shape pool; params become the shape's bounding half extents.
    u32 addPooledShape(CollisionShapeType type, u32 bodyIndex, u32 ref);
    /// Moves the last shape into `index` and shrinks by one.
    void removeShapeSwap(u32 index);
};

} // namespace fuse::physics
