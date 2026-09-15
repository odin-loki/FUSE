#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/types.hpp>

namespace fuse::physics::narrowphase {

FUSE_PHYSICS_INLINE vec3 support(const vec3* vertices, u32 count, vec3 direction) {
    if (count == 0) {
        return {};
    }

    f32 maxDot = -1e30f;
    vec3 best = vertices[0];
    for (u32 i = 0; i < count; ++i) {
        const f32 dot = vertices[i].dot(direction);
        if (dot > maxDot) {
            maxDot = dot;
            best = vertices[i];
        }
    }
    return best;
}

/// GJK intersection test — returns true when convex hulls overlap (B4.3 stub).
bool gjkIntersect(const vec3* hullA, u32 countA, const vec3* hullB, u32 countB);

/// EPA stub — returns an invalid manifold until full B4.3 GPU path lands.
ContactManifold epa(
    const vec3* hullA,
    u32 countA,
    const vec3* hullB,
    u32 countB,
    u32 idxA,
    u32 idxB);

} // namespace fuse::physics::narrowphase
