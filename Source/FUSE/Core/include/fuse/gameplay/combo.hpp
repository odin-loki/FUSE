#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::gameplay {

struct ComboCounter {
    u32 count = 0;
    f32 window = 0.f;
    f32 timeLeft = 0.f;
    u32 maxCount = 0;
};

/// Returns the combo count after this hit.
/// timeLeft == 0 starts a new chain at 1. Count never exceeds maxCount, and the window reloads.
/// maxCount == 0 stays at 0. window <= 0 never chains: every hit returns 1 and timeLeft stays 0.
[[nodiscard]] inline u32 hit(ComboCounter& combo) {
    if (combo.maxCount == 0u) {
        combo.count = 0u;
        return 0u;
    }
    if (!std::isfinite(combo.window) || combo.window <= 0.f) {
        combo.count = 1u;
        combo.timeLeft = 0.f;
        return 1u;
    }
    if (!std::isfinite(combo.timeLeft) || combo.timeLeft <= 0.f) {
        combo.count = 1u;
    } else if (combo.count < combo.maxCount) {
        combo.count += 1u;
    } else {
        combo.count = combo.maxCount;
    }
    combo.timeLeft = combo.window;
    return combo.count;
}

/// Decays timeLeft. When it reaches 0, count is cleared. dt <= 0 does nothing.
inline void tick(ComboCounter& combo, f32 dt) {
    if (!std::isfinite(dt) || dt <= 0.f) {
        return;
    }
    if (!std::isfinite(combo.timeLeft) || combo.timeLeft <= dt) {
        combo.timeLeft = 0.f;
        combo.count = 0u;
        return;
    }
    combo.timeLeft -= dt;
}

} // namespace fuse::gameplay
