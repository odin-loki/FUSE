#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>
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
    const CollisionShapeSoA& shapes,
    NarrowphaseDispatchRejectReason expected);

/// Const preflight for narrowphase buffer dispatch (B4.6 deepen follow-up pass).
struct NarrowphaseDispatchPreflight {
    NarrowphaseDispatchRejectReason reason = NarrowphaseDispatchRejectReason::None;
    NarrowphaseBatchPreflight batch{};
    bool rejected = false;

    bool can_dispatch() const { return !rejected && reason == NarrowphaseDispatchRejectReason::None; }

/// Populate dispatch preflight without running shape dispatch (B4.6 deepen follow-up pass).
NarrowphaseDispatchPreflight preflight_run_narrowphase_into_buffer(

/// Returns true when narrowphase buffer dispatch should be skipped (B4.6 deepen follow-up pass).
bool can_skip_run_narrowphase_into_buffer(

/// Non-mutating dispatch predicate — mirrors `preflight_run_narrowphase_into_buffer` (B4.6 deepen follow-up pass).
bool should_run_narrowphase_into_buffer(

/// Const preflight for one narrowphase pair slot dispatch (B4.4 deepen guard pass).
struct NarrowphasePairSlotPreflight {
    ContactPairPreflight pairPreflight{};
    bool skipped = false;

    bool can_dispatch() const { return !skipped && pairPreflight.can_dispatch(); }

/// Populate pair-slot preflight without running shape dispatch (B4.4 deepen guard pass).
NarrowphasePairSlotPreflight preflight_narrowphase_pair_slot(
/// Const preflight for per-slot narrowphase dispatch (B4.6 deepen pass).
struct NarrowphaseSlotPreflight {
    ContactPairRejectReason reason = ContactPairRejectReason::None;

    bool can_dispatch() const { return !rejected; }

/// Populate per-slot narrowphase preflight without running shape dispatch (B4.6 deepen pass).
NarrowphaseSlotPreflight preflight_narrowphase_slot(
    const broadphase::CandidatePair& pair,

/// Returns true when narrowphase should skip this pair slot before dispatch (B4.4 deepen guard pass).
bool should_skip_narrowphase_pair_slot(

/// Non-mutating pair-slot predicate — inverse of `should_skip_narrowphase_pair_slot` (B4.4 deepen guard pass).
bool should_run_narrowphase_pair_slot(
/// Populate narrowphase run preflight without mutating buffers (B4.6 deepen pass).
NarrowphaseRunPreflight preflightNarrowphaseRun(

/// Returns true when narrowphase run should early-out before pair dispatch (B4.6 deepen pass).
bool canSkipNarrowphaseRun(

/// Const preflight for narrowphase-into-buffer dispatch (B4.6 deepen pass).
struct NarrowphaseIntoBufferPreflight {
    u32 pairCount = 0u;
    u32 dispatchableCount = 0u;
    bool canSkipPass = false;
    bool canSkipBufferCompaction = false;
    bool canSkipBufferClamp = false;

    bool can_dispatch() const { return pairCount > 0u && dispatchableCount > 0u; }

/// Populate narrowphase-into-buffer preflight without mutating the buffer (B4.6 deepen pass).
NarrowphaseIntoBufferPreflight preflight_narrowphase_into_buffer(
    const ContactBufferSoA& buffer);

/// Returns true when narrowphase-into-buffer should skip the full pass (B4.6 deepen pass).
bool can_skip_narrowphase_into_buffer(
    bool emptyPairs = false;

    bool can_run() const { return !emptyPairs; }

/// Populate narrowphase-into-buffer preflight without mutating the contact buffer (B4.6 deepen pass).
NarrowphaseIntoBufferPreflight preflightNarrowphaseIntoBuffer(

/// Returns true when narrowphase-into-buffer should early-out before pair dispatch (B4.6 deepen pass).
bool should_skip_narrowphase_into_buffer(
/// Const preflight for narrowphase buffer finalize (B4.6 deepen follow-up pass).
struct NarrowphaseBufferFinalizePreflight {
    u32 pairSlotCount = 0u;
    u32 validSlotCount = 0u;
    bool needsCompaction = false;
    bool needsClamp = false;

    bool can_finalize() const { return !skipped && pairSlotCount > 0u; }

/// Populate buffer finalize preflight without mutating the buffer (B4.6 deepen follow-up pass).
NarrowphaseBufferFinalizePreflight preflight_narrowphase_buffer_finalize(const ContactBufferSoA& buffer);

/// Returns true when buffer finalize should be skipped (B4.6 deepen follow-up pass).
bool can_skip_narrowphase_buffer_finalize(const ContactBufferSoA& buffer);

    bool can_run() const { return !emptyPairs && dispatchableCount > 0u; }




/// Returns true when extended deepen preflight rejects this slot (B4.6 deepen pass).
bool should_skip_narrowphase_slot_dispatch(
/// Read-only narrowphase dispatch diagnostics — no mutation (B4.5 deepen follow-up pass).

    bool can_dispatch() const { return !emptyPairs && batch.can_dispatch(); }
    bool can_skip() const { return emptyPairs || batch.can_skip(); }

/// Populate dispatch preflight without running shape dispatch (B4.5 deepen follow-up pass).
NarrowphaseDispatchPreflight preflight_narrowphase_dispatch(

/// Returns true when narrowphase dispatch should early-out before pair iteration (B4.5 deepen follow-up pass).
bool should_skip_narrowphase_dispatch(
/// Const preflight for job-safe narrowphase buffer dispatch (B4.6 deepen pass).
    bool emptyPairList = false;
    bool canSkipDispatch = false;

    bool can_dispatch() const { return !canSkipDispatch && batch.can_dispatch(); }

/// Populate narrowphase dispatch preflight without running shape tests (B4.6 deepen pass).

/// Returns true when narrowphase dispatch can early-out before shape tests (B4.6 deepen pass).
bool can_skip_narrowphase_dispatch(
/// True when narrowphase batch may early-out before slot preparation (B4.6 deepen pass).
/// Returns true when narrowphase buffer dispatch may proceed for at least one pair (B4.5 deepen follow-up pass).
/// Const preflight for narrowphase batch dispatch into a contact buffer (B4.6 deepen pass).
    bool canSkip = false;

    bool can_dispatch() const { return batch.can_dispatch(); }
    bool can_skip() const { return canSkip || batch.can_skip(); }

/// Populate dispatch preflight without running narrowphase (B4.6 deepen pass).

/// Returns true when narrowphase dispatch should be skipped entirely (B4.6 deepen pass).

/// Job-safe narrowphase: one output slot per candidate pair, then compact valid contacts.
void runNarrowphaseIntoBuffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer);

/// Narrowphase dispatch with batch and buffer preflight guards (B4.6 deepen pass).
void runNarrowphaseIntoBufferWithPreflight(
/// Early-out when batch preflight rejects all pairs; otherwise delegates to `runNarrowphaseIntoBuffer` (B4.6 deepen pass).
void runNarrowphaseIntoBufferIfDispatchable(
/// Job-safe narrowphase that skips pairs rejected by extended deepen preflight (B4.6 deepen pass).
void runNarrowphaseFilteredIntoBuffer(
/// Narrowphase dispatch with batch preflight early-out when all pairs are rejected (B4.6 deepen pass).
/// Job-safe narrowphase with beyond deepen preflight skip per pair (B4.6 deepen pass).
/// Existing `runNarrowphaseIntoBuffer` behavior is unchanged on valid paths.
void runNarrowphaseIntoBufferBeyond(
/// Dispatch only when preflight allows; returns false when skipped (B4.6 deepen follow-up pass).
bool run_narrowphase_into_buffer_with_preflight(
/// True when batch preflight reports no dispatchable pairs (B4.3 deepen pass).
bool can_skip_run_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Narrowphase dispatch only when batch preflight allows; no-op otherwise (B4.3 deepen pass).
void runNarrowphaseIntoBufferWithPreflight(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer);
/// Returns true when narrowphase buffer compaction/clamp can be skipped after dispatch (B4.5 deepen pass).
bool should_skip_narrowphase_buffer_pass(const ContactBufferSoA& buffer);

/// Returns true when beyond batch preflight reports no dispatchable pairs (B4.6 deepen pass).
bool can_skip_narrowphase_beyond(
/// Returns true when buffer dispatch may be skipped (empty pair list) (B4.6 deepen pass).
bool can_skip_narrowphase_buffer_dispatch(
    const std::vector<broadphase::CandidatePair>& pairs);

/// Returns true when deepen buffer dispatch may be skipped (empty or all pairs rejected) (B4.6 deepen pass).
bool can_skip_narrowphase_buffer_dispatch_deepen(
/// Const preflight for narrowphase into-buffer dispatch (B4.6 deepen pass).
struct NarrowphaseIntoBufferPreflight {
    NarrowphaseBatchPreflight batch{};
    bool skipped = false;

    bool can_run() const { return !skipped && batch.can_dispatch(); }
};

/// Populate into-buffer preflight without running shape dispatch (B4.6 deepen pass).
NarrowphaseIntoBufferPreflight preflight_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Const preflight for narrowphase buffer dispatch (B4.6 deepen pass).
struct NarrowphaseBufferDispatchPreflight {
    bool emptyPairs = false;
    bool allRejected = false;
    u32 pairCount = 0u;
    u32 dispatchableCount = 0u;

    bool can_dispatch() const { return !skipped; }

/// Populate buffer-dispatch preflight without running shape dispatch (B4.6 deepen pass).
NarrowphaseBufferDispatchPreflight preflight_narrowphase_buffer_dispatch(
    const CollisionShapeSoA& shapes,
    bool useDeepenReject = false);

/// Job-safe narrowphase with deepen pair-reject guards; valid pairs unchanged (B4.6 deepen pass).
void runNarrowphaseIntoBufferDeepen(
/// Returns true when into-buffer dispatch should be skipped entirely (B4.6 deepen pass).
bool can_skip_narrowphase_into_buffer(

/// Run narrowphase into buffer only when preflight allows; clears buffer when skipped (B4.6 deepen pass).
void run_narrowphase_into_buffer_with_preflight(
    ContactBufferSoA& buffer);

/// CPU stub of the CUDA narrow-phase dispatch (B4.3).
std::vector<ContactManifold> runNarrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Read-only narrowphase dispatch diagnostics — no mutation (B4.4 deepen pass).
struct NarrowphaseDispatchPreflight {
    NarrowphaseBatchPreflight batch{};
    ContactBufferCompactionPreflight compaction{};
    ContactBufferClampPreflight clamp{};
    bool skipped = false;

    bool can_dispatch() const { return !skipped && batch.can_dispatch(); }
    bool can_skip_buffer_passes() const {
        return compaction.reason != ContactBufferCompactionRejectReason::None &&
               clamp.reason != ContactBufferClampRejectReason::None;
    }
};

/// Populate dispatch preflight without running shape dispatch (B4.4 deepen pass).
NarrowphaseDispatchPreflight preflight_narrowphase_dispatch(
/// Const preflight for narrowphase buffer dispatch (B4.6 deepen follow-up pass).
    bool canSkipBufferPass = false;

    bool can_dispatch() const { return batch.can_dispatch(); }
    bool can_skip() const { return batch.can_skip(); }

/// Populate dispatch preflight without running shape dispatch (B4.6 deepen follow-up pass).
NarrowphaseDispatchPreflight preflight_run_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const ContactBufferSoA& buffer);

/// Non-mutating dispatch skip predicate — mirrors `can_skip_narrowphase` (B4.4 deepen pass).
bool can_skip_narrowphase_dispatch(
/// Read-only narrowphase pair-slot diagnostics — no mutation (B4.4 deepen guard pass).
struct NarrowphasePairSlotPreflight {
    bool pairRejected = false;
    bool canDispatch = false;
    bool canFinalize = false;
    bool needsFrictionBasis = false;

    bool can_process() const { return !skipped && canDispatch; }

/// Populate pair-slot preflight without running shape dispatch (B4.4 deepen guard pass).
NarrowphasePairSlotPreflight preflight_narrowphase_pair_slot(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Non-mutating pair-slot skip predicate — mirrors base pair reject only (B4.4 deepen guard pass).
bool should_skip_narrowphase_pair_slot(
/// Dispatch one pair slot with deepen preflight and finalize guards (B4.4 deepen pass).
/// Returns an invalid manifold when deepen preflight rejects the pair.
ContactManifold detect_contacts_pair_deepen(

/// Finalize one detected manifold when finalize preflight allows (B4.4 deepen pass).
bool finalize_contact_manifold_if_needed(ContactManifold& manifold);

/// Job-safe narrowphase with deepen pair preflight and finalize guards (B4.4 deepen pass).
void runNarrowphaseDeepenIntoBuffer(
    ContactBufferSoA& buffer);
/// Read-only narrowphase buffer dispatch diagnostics (B4.5 deepen pass).
    bool emptyPairs = false;
    bool allPairsDeepenRejected = false;
    u32 pairCount = 0u;
    u32 dispatchablePairCount = 0u;

    bool can_prepare_buffer() const { return !emptyPairs; }
    bool can_skip_entire_dispatch() const { return emptyPairs || allPairsDeepenRejected; }

/// Populate narrowphase dispatch preflight without mutating buffers (B4.5 deepen pass).

/// Returns true when pair-slot narrowphase dispatch may be skipped via deepen preflight (B4.5 deepen pass).
/// Narrowphase with deepen pair preflight; skips dispatch when all pairs are deepen-rejected (B4.4 deepen follow-up pass).
void runNarrowphaseIntoBufferDeepen(

/// Narrowphase with deepen pair preflight before per-pair dispatch (B4.5 deepen pass).
/// Skips pairs rejected by deepen preflight; does not alter `runNarrowphaseIntoBuffer` behavior.

/// CPU stub using deepen pair preflight (B4.5 deepen pass).
std::vector<ContactManifold> runNarrowphaseDeepen(

/// Narrowphase dispatch with extended deepen pair-reject preflight (B4.4 deepen pass follow-up).
/// Does not alter `runNarrowphaseIntoBuffer`; valid base-path pairs are unchanged on that entry.

/// Vector-return deepen dispatch stub mirroring `runNarrowphase` (B4.4 deepen pass follow-up).
/// Job-safe narrowphase with deepen preflight skip per pair (B4.6 deepen pass).
void runNarrowphaseIntoBufferWithDeepenPreflight(

/// Returns false when batch deepen preflight reports no dispatchable pairs (B4.6 deepen pass).
bool runNarrowphaseIntoBufferIfNeeded(

/// Job-safe narrowphase with extended deepen preflight gates (B4.6 deepen pass).

/// CPU stub with extended deepen preflight gates (B4.6 deepen pass).
std::vector<ContactManifold> runNarrowphaseWithDeepenPreflight(

/// Job-safe narrowphase with extended deepen preflight rejects (B4.6 deepen pass).

/// CPU stub with extended deepen preflight rejects (B4.6 deepen pass).
/// Const preflight for narrowphase buffer dispatch (B4.6 deepen pass).

    bool can_skip() const { return skipped || batch.can_skip(); }

/// Populate narrowphase dispatch preflight without running shape dispatch (B4.6 deepen pass).
NarrowphaseDispatchPreflight preflight_run_narrowphase_into_buffer(

/// Returns true when narrowphase dispatch preflight reports no dispatchable pairs (B4.6 deepen pass).
bool should_skip_narrowphase_dispatch(
/// Narrowphase dispatch using extended deepen pair preflight and contact-buffer guarded writes (B4.6 deepen pass).
/// Const preflight for one narrowphase pair slot (B4.6 deepen pass).
    ContactPairSlotPreflight pair{};

    bool can_dispatch() const { return !skipped && pair.can_dispatch(); }

/// Populate pair-slot preflight without running shape dispatch (B4.6 deepen pass).
    u32 pairIndex);

/// Returns true when narrowphase should skip this pair slot before dispatch (B4.6 deepen pass).

/// True when all pairs are rejected by second-layer deepen preflight or the pair list is empty (B4.6 narrowphase deepen pass).
bool can_skip_narrowphase_second(
/// Job-safe narrowphase with deepen pair-reject guards and finalize preflight (B4.6 deepen pass).
void runNarrowphaseIntoBufferWithDeepenGuards(

/// Job-safe narrowphase with extended deepen pair preflight guards (B4.6 deepen follow-up pass).

/// Returns true when all pairs would be deepen-rejected before narrowphase dispatch (B4.6 deepen follow-up pass).
bool can_skip_narrowphase_into_buffer(
/// Const preflight for job-safe narrowphase buffer dispatch (B4.5 deepen pass).
struct NarrowphaseIntoBufferPreflight {
    NarrowphaseBatchPreflight batchPreflight{};

    bool can_dispatch() const { return !emptyPairs; }
    bool can_skip() const { return emptyPairs; }

/// Populate narrowphase-into-buffer preflight without running shape dispatch (B4.5 deepen pass).
NarrowphaseIntoBufferPreflight preflight_narrowphase_into_buffer(

/// Returns true when narrowphase into buffer may skip pair dispatch (empty pair list only) (B4.5 deepen pass).

/// Returns true when narrowphase into buffer should run pair dispatch (B4.5 deepen pass).
bool should_run_narrowphase_into_buffer(
/// True when narrowphase pair dispatch can be skipped entirely (empty pair list) (B4.6 deepen pass).
bool can_skip_narrowphase_dispatch(const std::vector<broadphase::CandidatePair>& pairs);
/// Const preflight for narrowphase buffer output dispatch (B4.6 deepen pass).
struct NarrowphaseBufferDispatchPreflight {
    bool emptyPairList = false;
    bool canFinalizeBuffer = false;

    bool can_dispatch() const { return pairCount > 0u; }

/// Populate buffer-dispatch preflight without running shape dispatch (B4.6 deepen pass).
NarrowphaseBufferDispatchPreflight preflight_narrowphase_buffer_dispatch(
    const std::vector<broadphase::CandidatePair>& pairs);

/// Returns true when narrowphase buffer finalize should be skipped (B4.6 deepen pass).
bool can_skip_narrowphase_buffer_finalize(
/// Const preflight for narrowphase-into-buffer dispatch (B4.6 deepen pass).


/// Populate into-buffer preflight without mutating the contact buffer (B4.6 deepen pass).
FUSE_PHYSICS_INLINE NarrowphaseIntoBufferPreflight preflight_narrowphase_into_buffer(
    const CollisionShapeSoA& shapes) {
    NarrowphaseIntoBufferPreflight preflight{};
    preflight.batch = preflight_narrowphase_batch(pairs, bodies, shapes);
    preflight.skipped = preflight.batch.can_skip();
    return preflight;

/// Returns true when narrowphase-into-buffer should early-out (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool can_skip_narrowphase_into_buffer(
    return preflight_narrowphase_into_buffer(pairs, bodies, shapes).can_skip();

/// Narrowphase into buffer only when batch preflight reports dispatchable pairs (B4.6 deepen pass).
FUSE_PHYSICS_INLINE void runNarrowphaseIntoBufferIfNeeded(
    ContactBufferSoA& buffer) {
    if (can_skip_narrowphase_into_buffer(pairs, bodies, shapes)) {
        buffer.clear();
        return;
    runNarrowphaseIntoBuffer(pairs, bodies, shapes, buffer);
/// Const preflight for narrowphase batch dispatch into a contact buffer (B4.6 deepen pass).


/// Populate dispatch preflight without running shape dispatch (B4.6 deepen pass).
FUSE_PHYSICS_INLINE NarrowphaseDispatchPreflight preflight_run_narrowphase(
    NarrowphaseDispatchPreflight preflight{};

/// Returns true when narrowphase batch dispatch should be skipped (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool can_skip_run_narrowphase(
    return preflight_run_narrowphase(pairs, bodies, shapes).can_skip();

/// Returns true when narrowphase batch dispatch may proceed (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool should_run_narrowphase(
    return preflight_run_narrowphase(pairs, bodies, shapes).can_dispatch();

/// Job-safe narrowphase with batch preflight early-out when all pairs are rejected (B4.6 deepen pass).
FUSE_PHYSICS_INLINE void runNarrowphaseIntoBufferWithPreflight(
    const NarrowphaseDispatchPreflight preflight = preflight_run_narrowphase(pairs, bodies, shapes);
    const u32 pairCount = static_cast<u32>(pairs.size());
    if (!preflight.can_dispatch()) {
        buffer.preparePairSlots(pairCount);
        compact_and_clamp_contact_buffer_with_preflight(buffer);

FUSE_PHYSICS_INLINE NarrowphaseBufferDispatchPreflight preflight_narrowphase_buffer_dispatch(
    const std::vector<broadphase::CandidatePair>& pairs) {
    NarrowphaseBufferDispatchPreflight preflight{};
    preflight.pairCount = static_cast<u32>(pairs.size());
    preflight.emptyPairList = pairs.empty();
    preflight.canFinalizeBuffer = !preflight.emptyPairList;

FUSE_PHYSICS_INLINE bool can_skip_narrowphase_buffer_finalize(
    return preflight_narrowphase_buffer_dispatch(pairs).emptyPairList;
/// Export narrowphase results only when dispatch preflight allows (B4.6 deepen follow-up pass).
std::vector<ContactManifold> run_narrowphase_with_preflight(

FUSE_PHYSICS_INLINE const char* narrowphase_dispatch_reject_reason_name(
    NarrowphaseDispatchRejectReason reason) {
    switch (reason) {
    case NarrowphaseDispatchRejectReason::None:
        return "None";
    case NarrowphaseDispatchRejectReason::EmptyPairList:
        return "EmptyPairList";
    case NarrowphaseDispatchRejectReason::AllPairsRejected:
        return "AllPairsRejected";
    return "Unknown";

FUSE_PHYSICS_INLINE NarrowphaseDispatchRejectReason narrowphase_dispatch_reject_reason(
    if (pairs.empty()) {
        return NarrowphaseDispatchRejectReason::EmptyPairList;
    if (narrowphase_batch_rejects_all(pairs, bodies, shapes)) {
        return NarrowphaseDispatchRejectReason::AllPairsRejected;
    return NarrowphaseDispatchRejectReason::None;

FUSE_PHYSICS_INLINE bool narrowphase_dispatch_rejects_for_reason(
    NarrowphaseDispatchRejectReason expected) {
    return narrowphase_dispatch_reject_reason(pairs, bodies, shapes) == expected;

FUSE_PHYSICS_INLINE NarrowphaseDispatchPreflight preflight_run_narrowphase_into_buffer(
    preflight.reason = narrowphase_dispatch_reject_reason(pairs, bodies, shapes);
    preflight.rejected = preflight.reason != NarrowphaseDispatchRejectReason::None;

FUSE_PHYSICS_INLINE bool can_skip_run_narrowphase_into_buffer(
    return !preflight_run_narrowphase_into_buffer(pairs, bodies, shapes).can_dispatch();

FUSE_PHYSICS_INLINE bool should_run_narrowphase_into_buffer(
    return preflight_run_narrowphase_into_buffer(pairs, bodies, shapes).can_dispatch();

FUSE_PHYSICS_INLINE bool run_narrowphase_into_buffer_with_preflight(
    if (!should_run_narrowphase_into_buffer(pairs, bodies, shapes)) {
        return false;
    return true;

FUSE_PHYSICS_INLINE std::vector<ContactManifold> run_narrowphase_with_preflight(
        return {};
    return runNarrowphase(pairs, bodies, shapes);
/// Non-mutating narrowphase-into-buffer skip predicate using batch preflight (B4.6 deepen follow-up pass).

/// Non-mutating narrowphase-into-buffer predicate — inverse of skip predicate (B4.6 deepen follow-up pass).
/// Run narrowphase only when dispatch preflight reports dispatchable pairs (B4.6 deepen pass).
inline void runNarrowphaseIntoBufferIfNeeded(
    if (can_skip_narrowphase_dispatch(pairs, bodies, shapes)) {

inline NarrowphaseDispatchPreflight preflight_narrowphase_dispatch(
    preflight.canSkip = preflight.batch.can_skip();

inline bool can_skip_narrowphase_dispatch(
    return preflight_narrowphase_dispatch(pairs, bodies, shapes).can_skip();
struct NarrowphaseBufferPreflight {
    ContactBufferCompactAndClampPreflight bufferPostPass{};
    bool allPairsRejected = false;
    bool bufferAlreadyPacked = false;

    bool can_dispatch() const { return batch.can_dispatch(); }

    bool can_skip() const {
        return emptyPairList || allPairsRejected;

/// Populate narrowphase buffer preflight without running dispatch (B4.6 deepen pass).
inline NarrowphaseBufferPreflight preflight_run_narrowphase_into_buffer(
    const ContactBufferSoA& buffer) {
    NarrowphaseBufferPreflight preflight{};
    preflight.allPairsRejected = !preflight.batch.can_dispatch();
    preflight.bufferPostPass = preflightContactBufferCompactAndClamp(buffer);
    preflight.bufferAlreadyPacked = buffer.canSkipCompactAndClamp();

/// Returns true when narrowphase buffer dispatch should be skipped (B4.6 deepen pass).
inline bool can_skip_run_narrowphase_into_buffer(
    return can_skip_narrowphase_deepen_pass(pairs, bodies, shapes);

/// Returns true when narrowphase buffer dispatch may proceed (B4.6 deepen pass).
inline bool should_run_narrowphase_into_buffer(
    return !can_skip_run_narrowphase_into_buffer(pairs, bodies, shapes);

/// Run narrowphase into buffer only when deepen-pass preflight allows (B4.6 deepen pass).
inline void run_narrowphase_into_buffer_with_preflight(
    if (can_skip_run_narrowphase_into_buffer(pairs, bodies, shapes)) {
/// Returns true when narrowphase buffer dispatch should be skipped (B4.6 deepen follow-up pass).
bool can_skip_run_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const ContactBufferSoA& buffer = ContactBufferSoA{});

/// Run narrowphase into buffer only when dispatch preflight allows; clears buffer otherwise (B4.6 deepen follow-up pass).
void run_narrowphase_into_buffer_with_preflight(

} // namespace fuse::physics::narrowphase
