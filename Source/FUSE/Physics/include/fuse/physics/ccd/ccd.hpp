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
    if (toi < 0.f || toi > 1.f) {
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
    bool shapeIsSphere(const CollisionShapeSoA& shapes, u32 shapeIndex, f32& outRadius) const;

    mutable u32 lastResultCount_ = 0;
};

} // namespace fuse::physics
