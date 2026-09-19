#pragma once

#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fuse::physics::narrowphase {

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
        return invalidContactManifold();
    }

    vec3 normal{};
    if (dist > 1e-6f) {
        normal = diff * (1.f / dist);
    } else {
        normal = {0.f, 1.f, 0.f};
    }

    ContactManifold manifold{};
    manifold.contactNormal = normal;
    manifold.minSeparation = sumRadius;
    manifold.bodyA = idxA;
    manifold.bodyB = idxB;
    manifold.valid = true;
    manifold.addPoint(
        {
            posB.x + normal.x * radiusB,
            posB.y + normal.y * radiusB,
            posB.z + normal.z * radiusB,
        },
        sumRadius - dist);
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
        return invalidContactManifold();
    }

    ContactManifold manifold{};
    manifold.contactNormal = planeNormal;
    manifold.minSeparation = sphereRadius;
    manifold.bodyA = idxSphere;
    manifold.bodyB = idxPlane;
    manifold.valid = true;
    manifold.addPoint(
        {
            spherePos.x - planeNormal.x * sphereRadius,
            spherePos.y - planeNormal.y * sphereRadius,
            spherePos.z - planeNormal.z * sphereRadius,
        },
        sphereRadius - dist);
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
        return invalidContactManifold();
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
    manifold.contactNormal = normal;
    manifold.minSeparation = sphereRadius;
    manifold.bodyA = idxSphere;
    manifold.bodyB = idxBox;
    manifold.valid = true;
    manifold.addPoint(spherePos - normal * sphereRadius, penetration);
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
        return invalidContactManifold();
    }

    vec3 normal{};
    if (dist > 1e-6f) {
        normal = diff * (1.f / dist);
    } else {
        normal = {0.f, 1.f, 0.f};
    }

    ContactManifold manifold{};
    manifold.contactNormal = normal;
    manifold.minSeparation = sumRadius;
    manifold.bodyA = idxSphere;
    manifold.bodyB = idxCapsule;
    manifold.valid = true;
    manifold.addPoint(axisPoint + normal * capsuleRadius, sumRadius - dist);
    return manifold;
}

/// Axis-aligned box vs box (stub ignores orientation; emits up to four face contact points).
ContactManifold collideBoxBox(
    vec3 posA,
    vec3 halfExtentsA,
    vec3 posB,
    vec3 halfExtentsB,
    u32 idxA,
    u32 idxB);

/// Why narrowphase buffer dispatch would early-out (B4.6 deepen follow-up pass).
enum class NarrowphaseDispatchRejectReason : u8 {
    None = 0,
    EmptyPairList,
    AllPairsRejected,
};

/// Human-readable label for narrowphase dispatch reject reasons (logging / tests).
const char* narrowphase_dispatch_reject_reason_name(NarrowphaseDispatchRejectReason reason);

/// Diagnose why runNarrowphaseIntoBuffer would skip; vacuously succeeds when dispatch may proceed.
NarrowphaseDispatchRejectReason narrowphase_dispatch_reject_reason(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when `narrowphase_dispatch_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool narrowphase_dispatch_rejects_for_reason(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    NarrowphaseDispatchRejectReason expected);

/// Const preflight for narrowphase buffer dispatch (B4.6 deepen follow-up pass).
struct NarrowphaseDispatchPreflight {
    NarrowphaseDispatchRejectReason reason = NarrowphaseDispatchRejectReason::None;
    NarrowphaseBatchPreflight batch{};
    bool rejected = false;

    bool can_dispatch() const { return !rejected && reason == NarrowphaseDispatchRejectReason::None; }
};

/// Populate dispatch preflight without running shape dispatch (B4.6 deepen follow-up pass).
NarrowphaseDispatchPreflight preflight_run_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when narrowphase buffer dispatch should be skipped (B4.6 deepen follow-up pass).
bool can_skip_run_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating dispatch predicate — mirrors `preflight_run_narrowphase_into_buffer` (B4.6 deepen follow-up pass).
bool should_run_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Job-safe narrowphase: one output slot per candidate pair, then compact valid contacts.
void runNarrowphaseIntoBuffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer);

/// Dispatch only when preflight allows; returns false when skipped (B4.6 deepen follow-up pass).
bool run_narrowphase_into_buffer_with_preflight(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer);

/// CPU stub of the CUDA narrow-phase dispatch (B4.3).
std::vector<ContactManifold> runNarrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Export narrowphase results only when dispatch preflight allows (B4.6 deepen follow-up pass).
std::vector<ContactManifold> run_narrowphase_with_preflight(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

FUSE_PHYSICS_INLINE const char* narrowphase_dispatch_reject_reason_name(
    NarrowphaseDispatchRejectReason reason) {
    switch (reason) {
    case NarrowphaseDispatchRejectReason::None:
        return "None";
    case NarrowphaseDispatchRejectReason::EmptyPairList:
        return "EmptyPairList";
    case NarrowphaseDispatchRejectReason::AllPairsRejected:
        return "AllPairsRejected";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE NarrowphaseDispatchRejectReason narrowphase_dispatch_reject_reason(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (pairs.empty()) {
        return NarrowphaseDispatchRejectReason::EmptyPairList;
    }
    if (narrowphase_batch_rejects_all(pairs, bodies, shapes)) {
        return NarrowphaseDispatchRejectReason::AllPairsRejected;
    }
    return NarrowphaseDispatchRejectReason::None;
}

FUSE_PHYSICS_INLINE bool narrowphase_dispatch_rejects_for_reason(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    NarrowphaseDispatchRejectReason expected) {
    return narrowphase_dispatch_reject_reason(pairs, bodies, shapes) == expected;
}

FUSE_PHYSICS_INLINE NarrowphaseDispatchPreflight preflight_run_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    NarrowphaseDispatchPreflight preflight{};
    preflight.reason = narrowphase_dispatch_reject_reason(pairs, bodies, shapes);
    preflight.batch = preflight_narrowphase_batch(pairs, bodies, shapes);
    preflight.rejected = preflight.reason != NarrowphaseDispatchRejectReason::None;
    return preflight;
}

FUSE_PHYSICS_INLINE bool can_skip_run_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !preflight_run_narrowphase_into_buffer(pairs, bodies, shapes).can_dispatch();
}

FUSE_PHYSICS_INLINE bool should_run_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return preflight_run_narrowphase_into_buffer(pairs, bodies, shapes).can_dispatch();
}

FUSE_PHYSICS_INLINE bool run_narrowphase_into_buffer_with_preflight(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer) {
    if (!should_run_narrowphase_into_buffer(pairs, bodies, shapes)) {
        buffer.clear();
        return false;
    }
    runNarrowphaseIntoBuffer(pairs, bodies, shapes, buffer);
    return true;
}

FUSE_PHYSICS_INLINE std::vector<ContactManifold> run_narrowphase_with_preflight(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (!should_run_narrowphase_into_buffer(pairs, bodies, shapes)) {
        return {};
    }
    return runNarrowphase(pairs, bodies, shapes);
}

} // namespace fuse::physics::narrowphase
