#pragma once

// Pulls a pickup toward the player. The returned delta is the step the pickup
// takes: the direction from the pickup to the player, scaled by
// strength * (1 - distance / radius). Outside the radius, on top of the player,
// or with a non-positive radius or strength, the pickup does not move.
// Any non-finite input returns zero.

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <cmath>

namespace fuse::gameplay {

[[nodiscard]] inline fuse::math::Vec3 pickup_magnet_delta(fuse::math::Vec3 player, fuse::math::Vec3 pickup,
                                                          f32 radius, f32 strength) {
    const auto finiteVec = [](fuse::math::Vec3 v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    if (!finiteVec(player) || !finiteVec(pickup) || !std::isfinite(radius) || !std::isfinite(strength)) {
        return {};
    }
    if (!(radius > 0.f) || !(strength > 0.f)) {
        return {};
    }

    const fuse::math::Vec3 towardPlayer = player - pickup;
    const f32 distance = towardPlayer.length();
    // Direction is undefined when the pickup sits on the player.
    if (!std::isfinite(distance) || distance > radius || !(distance > 1e-8f)) {
        return {};
    }

    const f32 falloff = strength * (1.f - distance / radius);
    return towardPlayer * (falloff / distance);
}

} // namespace fuse::gameplay
