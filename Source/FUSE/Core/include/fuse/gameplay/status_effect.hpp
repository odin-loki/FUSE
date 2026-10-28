#pragma once

#include <fuse/gameplay/health.hpp>

#include <cmath>

namespace fuse::gameplay {

struct DamageOverTime {
    f32 damagePerSec = 0.f;
    f32 remaining = 0.f;
};

/// Applies damagePerSec * dt through apply_damage when dt and remaining are positive,
/// then consumes dt from remaining (clamped at 0). Returns the damage dealt this tick.
/// Non-positive damagePerSec deals 0 and still consumes duration. A non-positive or
/// non-finite dt leaves the effect and the pool unchanged.
[[nodiscard]] inline f32 tick_dot(DamageOverTime& effect, HealthPool& pool, f32 dt) {
    if (!std::isfinite(dt) || dt <= 0.f || !(effect.remaining > 0.f)) {
        return 0.f;
    }
    f32 dealt = 0.f;
    if (effect.damagePerSec > 0.f) {
        dealt = apply_damage(pool, effect.damagePerSec * dt);
    }
    effect.remaining -= dt;
    if (effect.remaining < 0.f) {
        effect.remaining = 0.f;
    }
    return dealt;
}

} // namespace fuse::gameplay
