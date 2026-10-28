#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

/// Schlick Fresnel: F = f0 + (1 - f0) * pow(1 - saturate(nDotV), 5).
/// saturate clamps nDotV to 0..1. Non-finite f0 or nDotV returns f0 when every
/// f0 channel is finite, otherwise zero.
[[nodiscard]] inline math::Vec3 schlick_fresnel(math::Vec3 f0, f32 nDotV) {
    const bool f0Finite = std::isfinite(f0.x) && std::isfinite(f0.y) && std::isfinite(f0.z);
    if (!f0Finite || !std::isfinite(nDotV)) {
        return f0Finite ? f0 : math::Vec3{};
    }
    const f32 clamped = std::clamp(nDotV, 0.f, 1.f);
    const f32 base = 1.f - clamped;
    const f32 base2 = base * base;
    const f32 weight = base2 * base2 * base;
    // f0 * (1 - w) + w matches f0 + (1 - f0) * w and is exactly f0 at w = 0 and 1 at w = 1.
    const f32 remain = 1.f - weight;
    return {f0.x * remain + weight, f0.y * remain + weight, f0.z * remain + weight};
}

} // namespace fuse::renderer
