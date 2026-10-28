#pragma once

// Experience curve. xp_to_advance(level) is the xp required to leave a 1-based level:
// base * growth^(level-1). Levels below 1, or at/above maxLevel, do not advance (0).
// A non-finite curve, base <= 0, or growth <= 0 also yields 0.
// level_for_total_xp walks from level 1, summing those costs until the next cost would
// pass totalXp. Negative or non-finite totals stay at level 1. The result is clamped
// to maxLevel.

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::gameplay {

struct ExperienceCurve {
    f32 base = 100.f;
    f32 growth = 1.5f;
    u32 maxLevel = 50;
};

/// Xp required to leave `level` (1-based). 0 means there is no further advance.
[[nodiscard]] inline f32 xp_to_advance(const ExperienceCurve& curve, u32 level) {
    if (level < 1u || level >= curve.maxLevel) {
        return 0.f;
    }
    if (!std::isfinite(curve.base) || !std::isfinite(curve.growth) || curve.base <= 0.f ||
        curve.growth <= 0.f) {
        return 0.f;
    }
    // Integer power: growth^(level-1). Repeated multiply matches the formula for a
    // whole-number exponent and stays deterministic across libm implementations.
    f32 scale = 1.f;
    for (u32 i = 1u; i < level; ++i) {
        scale *= curve.growth;
        if (!std::isfinite(scale)) {
            return 0.f;
        }
    }
    const f32 xp = curve.base * scale;
    if (!std::isfinite(xp) || xp <= 0.f) {
        return 0.f;
    }
    return xp;
}

/// Highest level whose advance costs are covered by `totalXp`.
[[nodiscard]] inline u32 level_for_total_xp(const ExperienceCurve& curve, f32 totalXp) {
    if (!std::isfinite(totalXp) || totalXp < 0.f) {
        return 1u;
    }
    u32 level = 1u;
    f32 spent = 0.f;
    while (level < curve.maxLevel) {
        const f32 cost = xp_to_advance(curve, level);
        if (!std::isfinite(cost) || cost <= 0.f) {
            break;
        }
        if (spent + cost > totalXp) {
            break;
        }
        spent += cost;
        ++level;
    }
    if (level > curve.maxLevel) {
        return curve.maxLevel;
    }
    return level;
}

} // namespace fuse::gameplay
