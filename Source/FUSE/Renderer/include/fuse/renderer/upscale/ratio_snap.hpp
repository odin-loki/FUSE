#pragma once

// Snap a render/display ratio onto the fixed upscale presets 0.5, 0.67, 0.75 and 1.
// 0.67 is the stored preset (not 2/3). The closest preset wins; an equal distance picks the
// higher preset. A non-positive or non-finite ratio returns 1. Every other input is clamped
// to [0.5, 1] before that search.
//
// render_extent() is one axis of the render target: round(display * ratio), at least 1.
// display <= 0 returns 1. The Extent2D render_extent(display, mode) in upscaler.hpp is the
// quality-mode path and is a different overload.

#include <algorithm>
#include <cmath>

namespace fuse::renderer::upscale {

/// Closest preset in {0.5, 0.67, 0.75, 1}. Ties take the higher preset.
/// Non-positive or non-finite `ratio` returns 1. Other values are clamped to [0.5, 1] first.
[[nodiscard]] inline float snap_upscale_ratio(float ratio) {
    if (!(ratio > 0.f) || !std::isfinite(ratio)) {
        return 1.f;
    }
    const float clamped = std::clamp(ratio, 0.5f, 1.f);
    // Ascending, so `dist <= best_dist` keeps the higher preset on a tie.
    constexpr float kPresets[] = {0.5f, 0.67f, 0.75f, 1.f};
    float best = kPresets[0];
    double best_dist = std::fabs(static_cast<double>(clamped) - static_cast<double>(best));
    for (const float candidate : kPresets) {
        const double dist = std::fabs(static_cast<double>(clamped) - static_cast<double>(candidate));
        if (dist <= best_dist) {
            best = candidate;
            best_dist = dist;
        }
    }
    return best;
}

/// round(display * ratio), at least 1. display <= 0 returns 1.
[[nodiscard]] inline int render_extent(int display, float ratio) {
    if (display <= 0) {
        return 1;
    }
    const double rounded = std::round(static_cast<double>(display) * static_cast<double>(ratio));
    if (!(rounded >= 1.0)) {
        return 1;
    }
    constexpr double kMaxInt = 2147483647.0;
    if (rounded > kMaxInt) {
        return static_cast<int>(kMaxInt);
    }
    return static_cast<int>(rounded);
}

} // namespace fuse::renderer::upscale
