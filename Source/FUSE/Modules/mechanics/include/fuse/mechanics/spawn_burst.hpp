#pragma once

// Spawn burst count. Folds a frame delta into leftover time and emits whole
// spawn intervals, capped per tick. Independent of SpawnComponent.

#include <cmath>

namespace fuse::mechanics {

/// Adds `dt` into `accumulator` and emits whole `interval` spawns.
/// `dt` is added only when it is positive and finite.
/// A non-positive or non-finite `interval` returns 0 and leaves `accumulator` unchanged.
/// `maxPerTick` below 1 returns 0 after that add, without emitting.
/// Emits at most `maxPerTick` whole intervals that fit, then subtracts
/// `interval * emitted` from `accumulator`.
[[nodiscard]] inline int spawn_burst_count(float& accumulator, float dt, float interval,
                                           int maxPerTick) {
    if (!std::isfinite(interval) || !(interval > 0.f)) {
        return 0;
    }
    if (std::isfinite(dt) && dt > 0.f) {
        accumulator += dt;
    }
    if (maxPerTick < 1) {
        return 0;
    }

    int emitted = 0;
    while (emitted < maxPerTick && accumulator >= interval) {
        accumulator -= interval;
        ++emitted;
    }
    return emitted;
}

} // namespace fuse::mechanics
