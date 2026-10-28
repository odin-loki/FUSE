#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <cmath>

namespace fuse::renderer {

/// Full-screen colour flash. `overlay` blends the base image toward `color` on the CPU, not in a shader.
struct ScreenFlash {
    fuse::math::Vec3 color{};
    f32 intensity = 0.f;
    f32 decayPerSec = 0.f;
};

namespace screen_flash_detail {

/// Non-finite values become 0. Finite values clamp to [0, 1].
[[nodiscard]] inline f32 clamp01(f32 v) {
    if (!std::isfinite(v) || v < 0.f) {
        return 0.f;
    }
    if (v > 1.f) {
        return 1.f;
    }
    return v;
}

} // namespace screen_flash_detail

/// Sets `color` and `intensity`, each clamped to [0, 1]. Non-finite components become 0.
inline void trigger(ScreenFlash& flash, fuse::math::Vec3 color, f32 intensity) {
    flash.color = {screen_flash_detail::clamp01(color.x), screen_flash_detail::clamp01(color.y),
                   screen_flash_detail::clamp01(color.z)};
    flash.intensity = screen_flash_detail::clamp01(intensity);
}

/// Multiplies intensity by exp(-decayPerSec * dt).
/// dt <= 0, decayPerSec <= 0, and a non-finite dt or decayPerSec leave intensity unchanged.
/// After a decay step, a non-finite or negative intensity clamps to 0.
inline void tick(ScreenFlash& flash, f32 dt) {
    if (!std::isfinite(dt) || !(dt > 0.f) || !std::isfinite(flash.decayPerSec) || !(flash.decayPerSec > 0.f)) {
        return;
    }
    flash.intensity *= std::exp(-flash.decayPerSec * dt);
    if (!std::isfinite(flash.intensity) || flash.intensity < 0.f) {
        flash.intensity = 0.f;
    }
}

/// base * (1 - intensity) + color * intensity, with each channel clamped to [0, 1].
[[nodiscard]] inline fuse::math::Vec3 overlay(fuse::math::Vec3 base, const ScreenFlash& flash) {
    const fuse::math::Vec3 mixed = base * (1.f - flash.intensity) + flash.color * flash.intensity;
    return {screen_flash_detail::clamp01(mixed.x), screen_flash_detail::clamp01(mixed.y),
            screen_flash_detail::clamp01(mixed.z)};
}

} // namespace fuse::renderer
