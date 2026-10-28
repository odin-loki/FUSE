#pragma once

#include <fuse/types.hpp>

#include <cmath>
#include <numbers>

namespace fuse::math {

/// Pull `currentYaw` toward `targetYaw` by at most `maxStep` radians.
///
/// Yaw is radians. The delta `targetYaw - currentYaw` is wrapped to (-pi, pi].
/// `strength` is clamped to [0, 1]. `maxStep < 0` is treated as 0.
/// step = clamp(delta * strength, -maxStep, maxStep). The return value is
/// `currentYaw + step` and is not rewrapped.
/// Any non-finite input returns `currentYaw` when that value is finite, otherwise 0.
[[nodiscard]] inline f32 aim_assist_yaw(f32 currentYaw, f32 targetYaw, f32 maxStep, f32 strength) {
    if (!std::isfinite(currentYaw) || !std::isfinite(targetYaw) || !std::isfinite(maxStep) ||
        !std::isfinite(strength)) {
        return std::isfinite(currentYaw) ? currentYaw : 0.f;
    }

    if (strength < 0.f) {
        strength = 0.f;
    } else if (strength > 1.f) {
        strength = 1.f;
    }
    if (maxStep < 0.f) {
        maxStep = 0.f;
    }

    constexpr f32 kPi = std::numbers::pi_v<f32>;
    constexpr f32 kTwoPi = 2.f * kPi;
    f32 delta = targetYaw - currentYaw;
    if (!std::isfinite(delta)) {
        return currentYaw;
    }
    // fmod brings the delta into (-2pi, 2pi). One adjustment then lands in (-pi, pi].
    delta = std::fmod(delta, kTwoPi);
    if (delta <= -kPi) {
        delta += kTwoPi;
    } else if (delta > kPi) {
        delta -= kTwoPi;
    }
    if (!std::isfinite(delta)) {
        return currentYaw;
    }

    f32 step = delta * strength;
    if (step > maxStep) {
        step = maxStep;
    } else if (step < -maxStep) {
        step = -maxStep;
    }
    return currentYaw + step;
}

} // namespace fuse::math
