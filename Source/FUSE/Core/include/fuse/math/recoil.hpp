#pragma once

#include <fuse/math/vec.hpp>

#include <cmath>

namespace fuse::math {

/// Deterministic recoil. `shotIndex` is 0-based and clamps to the last shot.
/// `shots == 0` or a non-finite coefficient returns the zero offset.
/// X is yaw; Y is pitch and grows upward.
struct RecoilPattern {
    f32 pitchPerShot = 0.f;
    f32 yawPerShot = 0.f;
    f32 yawAlternate = 0.f;
    u32 shots = 0;
};

/// Yaw (x) = yawPerShot * i + yawAlternate * ((i % 2 == 0) ? -1 : 1) * i.
/// Pitch (y) = pitchPerShot * i, where i is the clamped shot index.
[[nodiscard]] inline Vec2 recoil_offset(const RecoilPattern& pattern, u32 shotIndex) {
    if (pattern.shots == 0 || !std::isfinite(pattern.pitchPerShot) || !std::isfinite(pattern.yawPerShot) ||
        !std::isfinite(pattern.yawAlternate)) {
        return {};
    }
    if (shotIndex >= pattern.shots) {
        shotIndex = pattern.shots - 1u;
    }
    const f32 index = static_cast<f32>(shotIndex);
    const f32 yawSign = (shotIndex % 2u == 0u) ? -1.f : 1.f;
    return {pattern.yawPerShot * index + pattern.yawAlternate * yawSign * index, pattern.pitchPerShot * index};
}

} // namespace fuse::math
