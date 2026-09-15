#pragma once

#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/physics_data.hpp>

#include <fuse/types.hpp>

#include <cmath>
#include <vector>

namespace fuse::physics {

struct TOIResult {
    f32 toi = 0.f;
    vec3 contactPoint{};
    vec3 contactNormal{};
    u32 bodyA = 0;
    u32 bodyB = 0;
    bool valid = false;
};

struct ToiBufferSoA;

FUSE_PHYSICS_INLINE bool isToiInWindow(f32 toi) {
    return toi >= 0.f && toi <= 1.f;
}

FUSE_PHYSICS_INLINE TOIResult makeToiAtContact(f32 toi,
                                               vec3 contactPoint,
                                               vec3 contactNormal,
                                               u32 idxA,
                                               u32 idxB) {
    if (!isToiInWindow(toi)) {
        return {.valid = false};
    }

    return {
        .toi = toi,
        .contactPoint = contactPoint,
        .contactNormal = contactNormal,
        .bodyA = idxA,
        .bodyB = idxB,
        .valid = true,
    };
}

FUSE_PHYSICS_INLINE TOIResult sweptSphereSphere(vec3 posA0,
                                               vec3 velA,
                                               f32 radiusA,
                                               vec3 posB0,
                                               vec3 velB,
                                               f32 radiusB) {
    const vec3 relativePos = posA0 - posB0;
    const vec3 relativeVel = velA - velB;
    const f32 radiusSum = radiusA + radiusB;

    const f32 a = relativeVel.dot(relativeVel);
    const f32 b = 2.f * relativePos.dot(relativeVel);
    const f32 c = relativePos.dot(relativePos) - radiusSum * radiusSum;

    if (a < 1e-10f) {
        return {.valid = false};
    }

    const f32 discriminant = b * b - 4.f * a * c;
    if (discriminant < 0.f) {
        return {.valid = false};
    }

    const f32 sqrtDisc = std::sqrt(discriminant);
    const f32 t0 = (-b - sqrtDisc) / (2.f * a);
    const f32 t1 = (-b + sqrtDisc) / (2.f * a);

    const f32 toi = (t0 >= 0.f) ? t0 : t1;
    if (!isToiInWindow(toi)) {
        return {.valid = false};
    }

    const vec3 hitA = posA0 + velA * toi;
    const vec3 hitB = posB0 + velB * toi;

    return {
        .toi = toi,
        .contactPoint = hitB,
        .contactNormal = (hitA - hitB).normalized(),
        .bodyA = 0,
        .bodyB = 0,
        .valid = true,
    };
}

/// Sphere swept against infinite plane (`planeDistance` matches narrowphase sphere-plane convention).
FUSE_PHYSICS_INLINE TOIResult sweptSpherePlane(vec3 pos0,
                                              vec3 vel,
                                              f32 radius,
                                              vec3 planeNormal,
                                              f32 planeDistance) {
    const f32 signedDist0 = pos0.dot(planeNormal) - planeDistance;
    const f32 velAlongNormal = vel.dot(planeNormal);

    if (signedDist0 <= radius && signedDist0 >= -radius) {
        return makeToiAtContact(0.f, pos0 - planeNormal * radius, planeNormal, 0, 0);
    }

    if (std::fabs(velAlongNormal) < 1e-10f) {
        return {.valid = false};
    }

    if (signedDist0 > radius && velAlongNormal >= 0.f) {
        return {.valid = false};
    }

    const f32 toi = (radius - signedDist0) / velAlongNormal;
    if (!isToiInWindow(toi)) {
        return {.valid = false};
    }

    const vec3 hitCenter = pos0 + vel * toi;
    return makeToiAtContact(toi, hitCenter - planeNormal * radius, planeNormal, 0, 0);
}

FUSE_PHYSICS_INLINE TOIResult selectEarliestToi(const TOIResult& a, const TOIResult& b) {
    if (!a.valid) {
        return b;
    }
    if (!b.valid) {
        return a;
    }
    return (a.toi <= b.toi) ? a : b;
}

/// Thin axis-aligned slab along Z (half thickness in `slabHalfThickness`).
FUSE_PHYSICS_INLINE TOIResult sweptSphereSlabZ(vec3 pos0,
                                              vec3 vel,
                                              f32 radius,
                                              f32 slabCenterZ,
                                              f32 slabHalfThickness) {
    const f32 slabMin = slabCenterZ - slabHalfThickness;
    const f32 slabMax = slabCenterZ + slabHalfThickness;

    const TOIResult frontFace =
        sweptSpherePlane(pos0, vel, radius, {0.f, 0.f, 1.f}, slabMax);
    const TOIResult backFace =
        sweptSpherePlane(pos0, vel, radius, {0.f, 0.f, -1.f}, -slabMin);
    return selectEarliestToi(frontFace, backFace);
}

/// Job-safe CCD: one output slot per candidate pair, then compact valid TOIs.
void runCcdIntoBuffer(const std::vector<broadphase::CandidatePair>& pairs,
                      const RigidBodySoA& bodies,
                      const CollisionShapeSoA& shapes,
                      f32 dt,
                      ToiBufferSoA& buffer);

/// B4.6 — CCD dispatch stub (pair sweep + RB_CCD flag filter).
class CcdPipeline {
public:
    u32 sweepPairs(const RigidBodySoA& bodies,
                   const CollisionShapeSoA& shapes,
                   const std::vector<broadphase::CandidatePair>& pairs,
                   f32 dt,
                   std::vector<TOIResult>& outResults) const;

    u32 resultCount() const { return lastResultCount_; }

private:
    mutable u32 lastResultCount_ = 0;
};

} // namespace fuse::physics
