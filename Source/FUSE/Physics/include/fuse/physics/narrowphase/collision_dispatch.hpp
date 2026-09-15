#pragma once

#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fuse::physics::narrowphase {

struct ContactManifold {
    vec3 contactPoint{};
    vec3 contactNormal{};
    f32 penetrationDepth = 0.f;
    f32 minSeparation = 0.f;
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
    manifold.minSeparation = sumRadius;
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
    manifold.minSeparation = sphereRadius;
    manifold.bodyA = idxSphere;
    manifold.bodyB = idxPlane;
    manifold.valid = true;
    return manifold;
}

/// Axis-aligned box vs sphere (box half extents in `boxHalfExtents`, stub ignores orientation).
FUSE_PHYSICS_INLINE ContactManifold collideBoxSphere(
    vec3 spherePos,
    f32 sphereRadius,
    vec3 boxPos,
    vec3 boxHalfExtents,
    u32 idxSphere,
    u32 idxBox) {
    const vec3 local = spherePos - boxPos;
    const vec3 closest = {
        std::max(-boxHalfExtents.x, std::min(local.x, boxHalfExtents.x)),
        std::max(-boxHalfExtents.y, std::min(local.y, boxHalfExtents.y)),
        std::max(-boxHalfExtents.z, std::min(local.z, boxHalfExtents.z)),
    };

    const vec3 delta = local - closest;
    const f32 distSq = delta.dot(delta);
    if (distSq > sphereRadius * sphereRadius) {
        return {};
    }

    vec3 normal{};
    f32 penetration = 0.f;
    if (distSq > 1e-12f) {
        const f32 dist = std::sqrt(distSq);
        normal = delta * (1.f / dist);
        penetration = sphereRadius - dist;
    } else {
        const f32 penX = boxHalfExtents.x - std::fabs(local.x);
        const f32 penY = boxHalfExtents.y - std::fabs(local.y);
        const f32 penZ = boxHalfExtents.z - std::fabs(local.z);
        if (penX <= penY && penX <= penZ) {
            normal = {local.x >= 0.f ? 1.f : -1.f, 0.f, 0.f};
            penetration = penX + sphereRadius;
        } else if (penY <= penZ) {
            normal = {0.f, local.y >= 0.f ? 1.f : -1.f, 0.f};
            penetration = penY + sphereRadius;
        } else {
            normal = {0.f, 0.f, local.z >= 0.f ? 1.f : -1.f};
            penetration = penZ + sphereRadius;
        }
    }

    ContactManifold manifold{};
    manifold.contactPoint = spherePos - normal * sphereRadius;
    manifold.contactNormal = normal;
    manifold.penetrationDepth = penetration;
    manifold.minSeparation = sphereRadius;
    manifold.bodyA = idxSphere;
    manifold.bodyB = idxBox;
    manifold.valid = true;
    return manifold;
}

/// Y-axis capsule vs sphere (`capsuleParams.x` = radius, `capsuleParams.y` = half height).
FUSE_PHYSICS_INLINE ContactManifold collideCapsuleSphere(
    vec3 spherePos,
    f32 sphereRadius,
    vec3 capsulePos,
    vec3 capsuleParams,
    u32 idxSphere,
    u32 idxCapsule) {
    const f32 capsuleRadius = capsuleParams.x;
    const f32 halfHeight = capsuleParams.y;
    const vec3 segmentA = capsulePos - vec3{0.f, halfHeight, 0.f};
    const vec3 segmentB = capsulePos + vec3{0.f, halfHeight, 0.f};
    const vec3 segment = segmentB - segmentA;
    const f32 segmentLenSq = segment.dot(segment);

    vec3 axisPoint = capsulePos;
    if (segmentLenSq > 1e-12f) {
        const f32 t = std::max(0.f, std::min(1.f, (spherePos - segmentA).dot(segment) / segmentLenSq));
        axisPoint = segmentA + segment * t;
    }

    const vec3 diff = spherePos - axisPoint;
    const f32 dist = diff.length();
    const f32 sumRadius = sphereRadius + capsuleRadius;
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
    manifold.contactPoint = axisPoint + normal * capsuleRadius;
    manifold.contactNormal = normal;
    manifold.penetrationDepth = sumRadius - dist;
    manifold.minSeparation = sumRadius;
    manifold.bodyA = idxSphere;
    manifold.bodyB = idxCapsule;
    manifold.valid = true;
    return manifold;
}

struct ContactBufferSoA;

/// Job-safe narrowphase: one output slot per candidate pair, then compact valid contacts.
void runNarrowphaseIntoBuffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer);

/// CPU stub of the CUDA narrow-phase dispatch (B4.3).
std::vector<ContactManifold> runNarrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

} // namespace fuse::physics::narrowphase
