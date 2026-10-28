#pragma once

#include <fuse/physics/types.hpp>

#include <cmath>

namespace fuse::physics {

[[nodiscard]] inline bool can_step_up(f32 obstacleHeight, f32 maxStep, f32 groundClearance) {
    if (!std::isfinite(obstacleHeight) || !std::isfinite(maxStep) || !std::isfinite(groundClearance) ||
        maxStep <= 0.f || obstacleHeight <= 0.f) {
        return false;
    }
    const f32 clearance = groundClearance < 0.f ? 0.f : groundClearance;
    return obstacleHeight > clearance && obstacleHeight <= maxStep;
}

[[nodiscard]] inline f32 step_up_delta(f32 obstacleHeight, f32 maxStep, f32 groundClearance) {
    return can_step_up(obstacleHeight, maxStep, groundClearance) ? obstacleHeight : 0.f;
}

} // namespace fuse::physics
