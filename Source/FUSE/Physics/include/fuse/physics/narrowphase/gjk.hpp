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

/// Point of the Minkowski difference A - B with the hull points that produced it.
struct CsoPoint {
    vec3 v{};
    vec3 a{};
    vec3 b{};
};

struct GjkSimplex {
    CsoPoint points[4]{};
    u32 count = 0;
};

/// GJK boolean query on convex vertex hulls (touching counts as intersecting).
/// `simplex` receives the terminating simplex (EPA seeds from it); `iterations` the loop count.
bool gjkIntersect(const vec3* hullA, u32 countA, const vec3* hullB, u32 countB, GjkSimplex* simplex,
                  u32* iterations = nullptr);
bool gjkIntersect(const vec3* hullA, u32 countA, const vec3* hullB, u32 countB);

struct EpaResult {
    bool intersecting = false;
    /// Unit normal pointing from B towards A (the engine's contact normal convention):
    /// translating A by `normal * depth` separates the hulls.
    vec3 normal{0.f, 1.f, 0.f};
    f32 depth = 0.f;
    vec3 pointA{}; // deepest point of A inside B
    vec3 pointB{}; // matching point on B's surface
    u32 iterations = 0;
    /// False when the Minkowski difference is flat (degenerate hulls): depth is then 0.
    bool converged = false;
};

/// GJK + expanding polytope: penetration normal/depth/witness points for overlapping hulls.
EpaResult epaPenetration(const vec3* hullA, u32 countA, const vec3* hullB, u32 countB);

/// EPA as a contact manifold (single deepest point); invalid when the hulls are separated.
ContactManifold epa(
    const vec3* hullA,
    u32 countA,
    const vec3* hullB,
    u32 countB,
    u32 idxA,
    u32 idxB);

} // namespace fuse::physics::narrowphase
