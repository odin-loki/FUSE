#pragma once

#include <fuse/physics/types.hpp>

#include <cmath>

namespace fuse::physics {

/// Per-voxel hit points. `threshold` is the original maximum and is unused by `apply`.
struct VoxelDamage {
    f32 health = 0.f;
    f32 threshold = 0.f;
};

/// Subtract `amount` from health, clamped at 0.
/// A non-positive or non-finite amount changes nothing.
/// Returns true only when this call drops health from above 0 to 0.
/// A voxel already at 0 returns false.
[[nodiscard]] inline bool apply(VoxelDamage& voxel, f32 amount) {
    if (!std::isfinite(amount) || amount <= 0.f) {
        return false;
    }
    const bool wasAboveZero = voxel.health > 0.f;
    voxel.health -= amount;
    if (voxel.health < 0.f) {
        voxel.health = 0.f;
    }
    return wasAboveZero && voxel.health <= 0.f;
}

/// True when health has been spent.
[[nodiscard]] inline bool broken(const VoxelDamage& voxel) {
    return voxel.health <= 0.f;
}

} // namespace fuse::physics
