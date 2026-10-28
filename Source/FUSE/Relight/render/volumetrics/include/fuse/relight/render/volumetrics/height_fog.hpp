// FUSE Relight: exponential height-fog extinction. Header-only. Density is constant at and below the
// base (the froxel medium in shaders/rl_vol_core.h) and decays above it.
#pragma once

#include <cmath>

namespace fuse::relight::render::volumetrics {

/// Extinction at `height`. Equals `density` at and below `baseHeight`. Above the base,
/// `density * exp(-falloff * (height - baseHeight))`. `falloff <= 0` keeps `density` (clamped at 0)
/// at every height. Non-positive `density` or any non-finite input returns 0. Never negative.
[[nodiscard]] inline float height_fog_density(float height, float baseHeight, float density, float falloff) {
    if (!std::isfinite(height) || !std::isfinite(baseHeight) || !std::isfinite(density) || !std::isfinite(falloff) ||
        !(density > 0.f)) {
        return 0.f;
    }
    if (!(falloff > 0.f) || height <= baseHeight) {
        return density;
    }
    const float value = density * std::exp(-falloff * (height - baseHeight));
    return value > 0.f ? value : 0.f;
}

} // namespace fuse::relight::render::volumetrics
