#pragma once

#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics::narrowphase {

struct ContactManifold {
    vec3 contactPoint{};
    vec3 contactNormal{};
    f32 penetrationDepth = 0.f;
    u32 bodyA = 0;
    u32 bodyB = 0;
    bool valid = false;
};

FUSE_PHYSICS_INLINE ContactManifold collideSphereSphere(
    vec3 posA,
    f32 radiusA,
    vec3 posB,
    f32 radiusB,
    u32 idxA,
    u32 idxB) {
    const vec3 diff = posA - posB;
    const f32 dist = diff.length();
    const f32 sumRadius = radiusA + radiusB;
    if (dist > sumRadius) {
        return {};
    }

    vec3 normal{};
    if (dist > 1e-6f) {
        normal = diff * (1.f / dist);
    } else {
        normal = {0.f, 1.f, 0.f};
    }

    ContactManifold manifold{};
    manifold.contactPoint = {
        posB.x + normal.x * radiusB,
        posB.y + normal.y * radiusB,
        posB.z + normal.z * radiusB,
    };
    manifold.contactNormal = normal;
    manifold.penetrationDepth = sumRadius - dist;
    manifold.bodyA = idxA;
    manifold.bodyB = idxB;
    manifold.valid = true;
    return manifold;
}

FUSE_PHYSICS_INLINE ContactManifold collideSpherePlane(
    vec3 spherePos,
    f32 sphereRadius,
    vec3 planeNormal,
    f32 planeDistance,
    u32 idxSphere,
    u32 idxPlane) {
    const f32 dist = spherePos.dot(planeNormal) - planeDistance;
    if (dist > sphereRadius) {
        return {};
    }

    ContactManifold manifold{};
    manifold.contactPoint = {
        spherePos.x - planeNormal.x * sphereRadius,
        spherePos.y - planeNormal.y * sphereRadius,
        spherePos.z - planeNormal.z * sphereRadius,
    };
    manifold.contactNormal = planeNormal;
    manifold.penetrationDepth = sphereRadius - dist;
    manifold.bodyA = idxSphere;
    manifold.bodyB = idxPlane;
    manifold.valid = true;
    return manifold;
}

/// CPU stub of the CUDA narrow-phase dispatch (B4.3).
std::vector<ContactManifold> runNarrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

} // namespace fuse::physics::narrowphase
