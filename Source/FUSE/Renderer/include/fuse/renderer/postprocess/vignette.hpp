#pragma once

#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

/// UV-space vignette gain. The centre (0.5, 0.5) returns 1. Roundness 0 uses a square radius
/// (max of the absolute offsets from centre) and roundness 1 uses a circle (Euclidean). Roundness
/// is clamped to 0..1 and blends those radii. factor = 1 - intensity * saturate(radius / 0.5),
/// with intensity clamped to 0..1. Non-finite inputs return 1. The result is clamped to 0..1.
[[nodiscard]] inline f32 vignette_factor(f32 uvX, f32 uvY, f32 intensity, f32 roundness) {
    if (!std::isfinite(uvX) || !std::isfinite(uvY) || !std::isfinite(intensity) ||
        !std::isfinite(roundness)) {
        return 1.f;
    }
    const f32 dx = uvX - 0.5f;
    const f32 dy = uvY - 0.5f;
    const f32 circle = std::sqrt(dx * dx + dy * dy);
    const f32 square = std::max(std::fabs(dx), std::fabs(dy));
    const f32 t = std::clamp(roundness, 0.f, 1.f);
    const f32 radius = square + (circle - square) * t;
    const f32 darkened = std::clamp(intensity, 0.f, 1.f) * std::clamp(radius / 0.5f, 0.f, 1.f);
    return std::clamp(1.f - darkened, 0.f, 1.f);
}

} // namespace fuse::renderer
