#include <fuse/physics/narrowphase/gjk.hpp>

#include <algorithm>

namespace fuse::physics::narrowphase {

namespace {

bool aabbOverlap(const vec3* hullA, u32 countA, const vec3* hullB, u32 countB) {
    vec3 minA{1e30f, 1e30f, 1e30f};
    vec3 maxA{-1e30f, -1e30f, -1e30f};
    vec3 minB{1e30f, 1e30f, 1e30f};
    vec3 maxB{-1e30f, -1e30f, -1e30f};

    for (u32 i = 0; i < countA; ++i) {
        minA.x = std::min(minA.x, hullA[i].x);
        minA.y = std::min(minA.y, hullA[i].y);
        minA.z = std::min(minA.z, hullA[i].z);
        maxA.x = std::max(maxA.x, hullA[i].x);
        maxA.y = std::max(maxA.y, hullA[i].y);
        maxA.z = std::max(maxA.z, hullA[i].z);
    }

    for (u32 i = 0; i < countB; ++i) {
        minB.x = std::min(minB.x, hullB[i].x);
        minB.y = std::min(minB.y, hullB[i].y);
        minB.z = std::min(minB.z, hullB[i].z);
        maxB.x = std::max(maxB.x, hullB[i].x);
        maxB.y = std::max(maxB.y, hullB[i].y);
        maxB.z = std::max(maxB.z, hullB[i].z);
    }

    return minA.x <= maxB.x && maxA.x >= minB.x && minA.y <= maxB.y && maxA.y >= minB.y &&
           minA.z <= maxB.z && maxA.z >= minB.z;
}

} // namespace

bool gjkIntersect(const vec3* hullA, u32 countA, const vec3* hullB, u32 countB) {
    if (countA == 0 || countB == 0) {
        return false;
    }

    vec3 direction = hullB[0] - hullA[0];
    if (direction.length() < 1e-6f) {
        return true;
    }

    for (u32 iteration = 0; iteration < 16; ++iteration) {
        const vec3 pointA = support(hullA, countA, direction);
        const vec3 pointB = support(hullB, countB, {-direction.x, -direction.y, -direction.z});
        const vec3 minkowski = pointA - pointB;
        if (minkowski.dot(direction) < 0.f) {
            return false;
        }
        direction = {-minkowski.x, -minkowski.y, -minkowski.z};
        if (direction.length() < 1e-6f) {
            return true;
        }
    }

    return aabbOverlap(hullA, countA, hullB, countB);
}

ContactManifold epa(
    const vec3* hullA,
    u32 countA,
    const vec3* hullB,
    u32 countB,
    u32 idxA,
    u32 idxB) {
    ContactManifold manifold{};
    manifold.bodyA = idxA;
    manifold.bodyB = idxB;
    manifold.valid = gjkIntersect(hullA, countA, hullB, countB);
    return manifold;
}

} // namespace fuse::physics::narrowphase
