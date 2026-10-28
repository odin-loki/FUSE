#pragma once

#include <fuse/math/falloff.hpp>
#include <fuse/types.hpp>

#include <cmath>

namespace fuse::gameplay {

enum class DamageFalloff { Linear, Smooth, InverseSquare };

/// Multiplies baseDamage by a distance falloff. Non-positive or non-finite base damage is 0.
[[nodiscard]] inline f32 damage_at_distance(f32 baseDamage, f32 distance, f32 range, DamageFalloff kind) {
    if (baseDamage <= 0.f || !std::isfinite(baseDamage)) {
        return 0.f;
    }
    f32 scale = 0.f;
    if (kind == DamageFalloff::Linear) {
        scale = fuse::math::falloff_linear(distance, range);
    } else if (kind == DamageFalloff::Smooth) {
        scale = fuse::math::falloff_smooth(distance, range);
    } else if (kind == DamageFalloff::InverseSquare) {
        scale = fuse::math::falloff_inverse_square(distance, range);
    }
    return baseDamage * scale;
}

} // namespace fuse::gameplay
