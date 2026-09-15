#include <fuse/physics/narrowphase/collision_dispatch.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics::narrowphase {

namespace {

struct Aabb {
    vec3 min{};
    vec3 max{};
};

Aabb makeAabb(vec3 center, vec3 halfExtents) {
    return {
        center - halfExtents,
        center + halfExtents,
    };
}

f32 overlapOnAxis(f32 minA, f32 maxA, f32 minB, f32 maxB) {
    return std::min(maxA, maxB) - std::max(minA, minB);
}

} // namespace

ContactManifold collideBoxBox(
    vec3 posA,
    vec3 halfExtentsA,
    vec3 posB,
    vec3 halfExtentsB,
    u32 idxA,
    u32 idxB) {
    const Aabb aabbA = makeAabb(posA, halfExtentsA);
    const Aabb aabbB = makeAabb(posB, halfExtentsB);

    const f32 overlapX = overlapOnAxis(aabbA.min.x, aabbA.max.x, aabbB.min.x, aabbB.max.x);
    const f32 overlapY = overlapOnAxis(aabbA.min.y, aabbA.max.y, aabbB.min.y, aabbB.max.y);
    const f32 overlapZ = overlapOnAxis(aabbA.min.z, aabbA.max.z, aabbB.min.z, aabbB.max.z);
    if (overlapX <= 0.f || overlapY <= 0.f || overlapZ <= 0.f) {
        return invalidContactManifold();
    }

    f32 penetration = overlapX;
    vec3 normal{posA.x >= posB.x ? 1.f : -1.f, 0.f, 0.f};
    if (overlapY < penetration) {
        penetration = overlapY;
        normal = {0.f, posA.y >= posB.y ? 1.f : -1.f, 0.f};
    }
    if (overlapZ < penetration) {
        penetration = overlapZ;
        normal = {0.f, 0.f, posA.z >= posB.z ? 1.f : -1.f};
    }

    const f32 overlapMinX = std::max(aabbA.min.x, aabbB.min.x);
    const f32 overlapMaxX = std::min(aabbA.max.x, aabbB.max.x);
    const f32 overlapMinY = std::max(aabbA.min.y, aabbB.min.y);
    const f32 overlapMaxY = std::min(aabbA.max.y, aabbB.max.y);
    const f32 overlapMinZ = std::max(aabbA.min.z, aabbB.min.z);
    const f32 overlapMaxZ = std::min(aabbA.max.z, aabbB.max.z);

    const f32 faceCoord =
        normal.x != 0.f ? (normal.x > 0.f ? aabbA.min.x : aabbA.max.x)
        : normal.y != 0.f ? (normal.y > 0.f ? aabbA.min.y : aabbA.max.y)
                          : (normal.z > 0.f ? aabbA.min.z : aabbA.max.z);

    ContactManifold manifold{};
    manifold.contactNormal = normal;
    manifold.minSeparation = 0.f;
    manifold.bodyA = idxA;
    manifold.bodyB = idxB;
    manifold.valid = true;

    vec3 corners[4]{};
    if (normal.x != 0.f) {
        corners[0] = {faceCoord, overlapMinY, overlapMinZ};
        corners[1] = {faceCoord, overlapMaxY, overlapMinZ};
        corners[2] = {faceCoord, overlapMaxY, overlapMaxZ};
        corners[3] = {faceCoord, overlapMinY, overlapMaxZ};
    } else if (normal.y != 0.f) {
        corners[0] = {overlapMinX, faceCoord, overlapMinZ};
        corners[1] = {overlapMaxX, faceCoord, overlapMinZ};
        corners[2] = {overlapMaxX, faceCoord, overlapMaxZ};
        corners[3] = {overlapMinX, faceCoord, overlapMaxZ};
    } else {
        corners[0] = {overlapMinX, overlapMinY, faceCoord};
        corners[1] = {overlapMaxX, overlapMinY, faceCoord};
        corners[2] = {overlapMaxX, overlapMaxY, faceCoord};
        corners[3] = {overlapMinX, overlapMaxY, faceCoord};
    }

    for (const vec3& point : corners) {
        manifold.addPoint(point, penetration);
    }

    return manifold;
}

} // namespace fuse::physics::narrowphase
