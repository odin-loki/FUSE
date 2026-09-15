#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <vector>

namespace fuse::physics::broadphase {

struct SpatialHashParams {
    f32 cellSize = 2.f;
    u32 tableSize = 1024;
    u32 bodyCount = 0;
};

struct CandidatePair {
    u32 bodyA = 0;
    u32 bodyB = 0;
};

FUSE_PHYSICS_INLINE u32 spatialHash(s32 cx, s32 cy, s32 cz, u32 tableSize) {
    constexpr u32 p1 = 73856093u;
    constexpr u32 p2 = 19349663u;
    constexpr u32 p3 = 83492791u;
    const u32 hash = (static_cast<u32>(cx * p1) ^ static_cast<u32>(cy * p2) ^ static_cast<u32>(cz * p3));
    return tableSize == 0 ? 0u : hash % tableSize;
}

FUSE_PHYSICS_INLINE ivec3 worldToCell(vec3 position, f32 cellSize) {
    const f32 invCell = cellSize > 0.f ? 1.f / cellSize : 1.f;
    return {
        static_cast<s32>(std::floor(position.x * invCell)),
        static_cast<s32>(std::floor(position.y * invCell)),
        static_cast<s32>(std::floor(position.z * invCell)),
    };
}

FUSE_PHYSICS_INLINE u32 spatialHash2D(s32 cx, s32 cy, u32 tableSize) {
    constexpr u32 p1 = 73856093u;
    constexpr u32 p2 = 19349663u;
    const u32 hash = static_cast<u32>(cx * p1) ^ static_cast<u32>(cy * p2);
    return tableSize == 0 ? 0u : hash % tableSize;
}

FUSE_PHYSICS_INLINE ivec2 worldToCell2D(vec2 position, f32 cellSize) {
    const f32 invCell = cellSize > 0.f ? 1.f / cellSize : 1.f;
    return {
        static_cast<s32>(std::floor(position.x * invCell)),
        static_cast<s32>(std::floor(position.y * invCell)),
    };
}

/// CPU stub of the CUDA broad-phase pipeline (B4.2).
std::vector<CandidatePair> runBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params);

/// 2D broad phase for World2D composition (xy plane only).
std::vector<CandidatePair> runBroadphase2D(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params);

} // namespace fuse::physics::broadphase
