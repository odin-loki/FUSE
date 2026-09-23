#pragma once

#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
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
    u32 idxB,
    f32 margin = 0.f) {
    const vec3 diff = posA - posB;
    const f32 dist = diff.length();
    const f32 sumRadius = radiusA + radiusB;
    if (dist > sumRadius + margin) {
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
    u32 idxPlane,
    f32 margin = 0.f) {
    const f32 dist = spherePos.dot(planeNormal) - planeDistance;
    if (dist > sphereRadius + margin) {
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

/// Axis-aligned box vs plane: deepest point along -normal (box orientation ignored like the other box stubs).
FUSE_PHYSICS_INLINE ContactManifold collideBoxPlane(
    vec3 boxPos,
    vec3 boxHalfExtents,
    vec3 planeNormal,
    f32 planeDistance,
    u32 idxBox,
    u32 idxPlane) {
    const f32 extent = std::fabs(boxHalfExtents.x * planeNormal.x) + std::fabs(boxHalfExtents.y * planeNormal.y) +
                       std::fabs(boxHalfExtents.z * planeNormal.z);
    const f32 dist = boxPos.dot(planeNormal) - planeDistance;
    if (dist > extent) {
        return invalidContactManifold();
    }

    ContactManifold manifold{};
    manifold.contactNormal = planeNormal;
    manifold.minSeparation = extent;
    manifold.bodyA = idxBox;
    manifold.bodyB = idxPlane;
    manifold.valid = true;
    manifold.addPoint(boxPos - planeNormal * extent, extent - dist);
    return manifold;
}

/// Axis-aligned box vs sphere (box half extents in `boxHalfExtents`, stub ignores orientation).
FUSE_PHYSICS_INLINE ContactManifold collideBoxSphere(
    vec3 spherePos,
    f32 sphereRadius,
    vec3 boxPos,
    vec3 boxHalfExtents,
    u32 idxSphere,
    u32 idxBox,
    f32 margin = 0.f) {
    const vec3 local = spherePos - boxPos;
    const vec3 closest = {
        std::max(-boxHalfExtents.x, std::min(local.x, boxHalfExtents.x)),
        std::max(-boxHalfExtents.y, std::min(local.y, boxHalfExtents.y)),
        std::max(-boxHalfExtents.z, std::min(local.z, boxHalfExtents.z)),
    };

    const vec3 delta = local - closest;
    const f32 distSq = delta.dot(delta);
    if (distSq > (sphereRadius + margin) * (sphereRadius + margin)) {
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
    u32 idxCapsule,
    f32 margin = 0.f) {
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
    if (dist > sumRadius + margin) {
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

/// Closest points between segments p1-q1 and p2-q2 (Ericson, RTCD 5.1.9); handles parallel
/// and zero-length segments. Returns the squared distance.
FUSE_PHYSICS_INLINE f32 closestPointsSegmentSegment(vec3 p1, vec3 q1, vec3 p2, vec3 q2, vec3& c1, vec3& c2) {
    constexpr f32 kEps = 1e-12f;
    const vec3 d1 = q1 - p1;
    const vec3 d2 = q2 - p2;
    const vec3 r = p1 - p2;
    const f32 a = d1.dot(d1);
    const f32 e = d2.dot(d2);
    const f32 f = d2.dot(r);
    f32 s = 0.f;
    f32 t = 0.f;
    if (a <= kEps && e <= kEps) {
        c1 = p1;
        c2 = p2;
        return (c1 - c2).dot(c1 - c2);
    }
    if (a <= kEps) {
        t = std::clamp(f / e, 0.f, 1.f);
    } else {
        const f32 c = d1.dot(r);
        if (e <= kEps) {
            s = std::clamp(-c / a, 0.f, 1.f);
        } else {
            const f32 b = d1.dot(d2);
            const f32 denom = a * e - b * b; // 0 when parallel: pick s = 0, fixed up below
            s = denom > kEps * a * e ? std::clamp((b * f - c * e) / denom, 0.f, 1.f) : 0.f;
            t = (b * s + f) / e;
            if (t < 0.f) {
                t = 0.f;
                s = std::clamp(-c / a, 0.f, 1.f);
            } else if (t > 1.f) {
                t = 1.f;
                s = std::clamp((b - c) / a, 0.f, 1.f);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
    return (c1 - c2).dot(c1 - c2);
}

/// Capsules given by segment endpoints and radii. The normal points from B towards A;
/// `minSeparation` is expressed against the body centres so centre-based solvers see the
/// true surface separation.
FUSE_PHYSICS_INLINE ContactManifold collideCapsuleSegments(
    vec3 a0, vec3 a1, f32 radiusA, vec3 centerA,
    vec3 b0, vec3 b1, f32 radiusB, vec3 centerB,
    u32 idxA, u32 idxB, f32 margin = 0.f) {
    vec3 ca{};
    vec3 cb{};
    const f32 distSq = closestPointsSegmentSegment(a0, a1, b0, b1, ca, cb);
    const f32 sumRadius = radiusA + radiusB;
    if (distSq > (sumRadius + margin) * (sumRadius + margin)) {
        return invalidContactManifold();
    }
    const f32 dist = std::sqrt(distSq);
    vec3 normal{};
    if (dist > 1e-6f) {
        normal = (ca - cb) * (1.f / dist);
    } else {
        // Axes intersect: push apart perpendicular to both axes (or to the one that exists).
        const vec3 dA = a1 - a0;
        const vec3 dB = b1 - b0;
        vec3 perp = dA.cross(dB);
        if (perp.dot(perp) < 1e-12f) {
            const vec3 axis = dA.dot(dA) > 1e-12f ? dA : dB;
            perp = axis.cross(std::fabs(axis.x) < 0.9f * axis.length() ? vec3{1.f, 0.f, 0.f} : vec3{0.f, 1.f, 0.f});
        }
        normal = perp.dot(perp) > 1e-12f ? perp.normalized() : vec3{0.f, 1.f, 0.f};
    }
    const f32 penetration = sumRadius - dist;

    ContactManifold manifold{};
    manifold.contactNormal = normal;
    manifold.minSeparation = (centerA - centerB).dot(normal) + penetration;
    manifold.bodyA = idxA;
    manifold.bodyB = idxB;
    manifold.valid = true;
    manifold.addPoint(cb + normal * radiusB, penetration);
    return manifold;
}

/// Y-axis capsules (`params.x` = radius, `params.y` = half height).
FUSE_PHYSICS_INLINE ContactManifold collideCapsuleCapsule(
    vec3 posA, vec3 paramsA, vec3 posB, vec3 paramsB, u32 idxA, u32 idxB, f32 margin = 0.f) {
    const vec3 upA{0.f, paramsA.y, 0.f};
    const vec3 upB{0.f, paramsB.y, 0.f};
    return collideCapsuleSegments(posA - upA, posA + upA, paramsA.x, posA, posB - upB, posB + upB, paramsB.x, posB,
                                  idxA, idxB, margin);
}

/// Sphere vs signed distance field. `sdf(vec3) -> f32` is negative inside. The contact normal is
/// the normalised central-difference gradient (points out of the field, towards the sphere), so
/// it varies smoothly wherever the field is smooth.
template <typename Sdf>
ContactManifold collideSphereSdf(vec3 spherePos, f32 sphereRadius, const Sdf& sdf, u32 idxSphere, u32 idxField,
                                 f32 gradientStep = 1e-3f) {
    const f32 distance = sdf(spherePos);
    if (distance > sphereRadius) {
        return invalidContactManifold();
    }
    const f32 h = gradientStep;
    const vec3 gradient{
        sdf(spherePos + vec3{h, 0.f, 0.f}) - sdf(spherePos - vec3{h, 0.f, 0.f}),
        sdf(spherePos + vec3{0.f, h, 0.f}) - sdf(spherePos - vec3{0.f, h, 0.f}),
        sdf(spherePos + vec3{0.f, 0.f, h}) - sdf(spherePos - vec3{0.f, 0.f, h}),
    };
    if (gradient.dot(gradient) < 1e-20f) {
        return invalidContactManifold();
    }
    const vec3 normal = gradient.normalized();
    ContactManifold manifold{};
    manifold.contactNormal = normal;
    manifold.minSeparation = sphereRadius;
    manifold.bodyA = idxSphere;
    manifold.bodyB = idxField;
    manifold.valid = true;
    manifold.addPoint(spherePos - normal * distance, sphereRadius - distance);
    return manifold;
}

/// Axis-aligned box vs box (emits up to four face contact points). Oriented boxes take
/// `collideOrientedBoxBox`.
ContactManifold collideBoxBox(
    vec3 posA,
    vec3 halfExtentsA,
    vec3 posB,
    vec3 halfExtentsB,
    u32 idxA,
    u32 idxB,
    f32 margin = 0.f);

/// Oriented box vs plane: up to four of the deepest corners below the plane (box A, plane B).
ContactManifold collideOrientedBoxPlane(
    vec3 boxPos,
    quat boxRotation,
    vec3 boxHalfExtents,
    vec3 planeNormal,
    f32 planeDistance,
    u32 idxBox,
    u32 idxPlane,
    f32 margin = 0.f);

/// Sphere (A) vs oriented box (B): the axis-aligned test run in the box frame.
ContactManifold collideOrientedBoxSphere(
    vec3 spherePos,
    f32 sphereRadius,
    vec3 boxPos,
    quat boxRotation,
    vec3 boxHalfExtents,
    u32 idxSphere,
    u32 idxBox,
    f32 margin = 0.f);

/// Oriented box vs oriented box: separating-axis test over the 15 axes. Face contacts clip the
/// incident face against the reference face (up to four points); edge contacts give one point.
/// The normal points from B towards A.
ContactManifold collideOrientedBoxBox(
    vec3 posA,
    quat rotationA,
    vec3 halfExtentsA,
    vec3 posB,
    quat rotationB,
    vec3 halfExtentsB,
    u32 idxA,
    u32 idxB,
    f32 margin = 0.f);

/// Capsule (local Y axis, `params.x` radius, `params.y` half height) vs plane: one point per cap
/// centre closer than the radius (capsule A, plane B).
ContactManifold collideCapsulePlane(
    vec3 capsulePos,
    quat capsuleRotation,
    vec3 capsuleParams,
    vec3 planeNormal,
    f32 planeDistance,
    u32 idxCapsule,
    u32 idxPlane,
    f32 margin = 0.f);

/// Capsule (A) vs oriented box (B): deepest point of the capsule axis against the box, plus the
/// cap centres touching along the same normal.
ContactManifold collideCapsuleBox(
    vec3 capsulePos,
    quat capsuleRotation,
    vec3 capsuleParams,
    vec3 boxPos,
    quat boxRotation,
    vec3 boxHalfExtents,
    u32 idxCapsule,
    u32 idxBox,
    f32 margin = 0.f);

/// One shape at an explicit pose (for queries away from the bodies' stored poses, e.g. CCD).
struct ShapeInstance {
    CollisionShapeType type = CollisionShapeType::Sphere;
    vec3 params{};
    f32 scalar = 0.f;
    vec3 position{};
    quat orientation{};
};

/// Shape-pair narrowphase at explicit poses (the dispatch `collidePairs` runs per pair). The normal
/// points from B towards A; invalid for unsupported pairs or shapes further apart than `margin`.
ContactManifold collideShapes(const ShapeInstance& a, const ShapeInstance& b, u32 idxA, u32 idxB, f32 margin = 0.f);

struct ContactBufferSoA;

/// Job-safe narrowphase: one output slot per candidate pair, then compact valid contacts.
void runNarrowphaseIntoBuffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer);

/// CPU stub of the CUDA narrow-phase dispatch (B4.3).
/// Narrowphase into a caller-owned vector (cleared first; capacity reused across frames). Unlike
/// `runNarrowphase` it keeps trigger and sleeping pairs: callers decide what to resolve.
///
/// `margin` > 0 also keeps speculative manifolds for shapes separated by less than `margin`:
/// their points carry negative penetration (the separation). A position solver only acts on them
/// once they penetrate, which keeps stacked bodies coupled within one substep.
void collidePairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    std::vector<ContactManifold>& out,
    f32 margin = 0.f);

std::vector<ContactManifold> runNarrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

} // namespace fuse::physics::narrowphase
