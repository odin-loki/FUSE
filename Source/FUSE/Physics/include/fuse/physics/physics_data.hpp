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
    Plane = 6
};

enum RigidBodyFlags : u32 {
    RB_STATIC = 1u << 0,
    RB_SLEEPING = 1u << 1,
    RB_TRIGGER = 1u << 2,
    RB_KINEMATIC = 1u << 3,
    RB_NO_GRAVITY = 1u << 4,
    RB_CCD = 1u << 5
};

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

    std::vector<vec3> predictedPositions;
    std::vector<quat> predictedOrientations;

    u32 count() const { return static_cast<u32>(positions.size()); }
    u32 capacity() const { return static_cast<u32>(positions.capacity()); }

    void reserve(u32 bodyCapacity);
    void clear();
    u32 addBody(vec3 position, f32 invMass, u32 bodyFlags = 0);
};

/// Collision shape descriptors stored in SoA layout (B4.1).
struct CollisionShapeSoA {
    std::vector<u32> types;
    std::vector<vec3> params;
    std::vector<f32> scalars;
    std::vector<u32> bodyIndices;

    u32 count() const { return static_cast<u32>(types.size()); }

    void clear();
    u32 addShape(CollisionShapeType type, u32 bodyIndex, vec3 shapeParams, f32 scalarParam = 0.f);
};

} // namespace fuse::physics
