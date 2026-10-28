#pragma once

// Play-session tick accumulator. Folds a frame delta into leftover time and
// consumes whole ticks. Independent of PlaySession.

#include <cmath>

namespace fuse::editor {

/// Adds `dt` into `accumulator` and consumes whole `tickSeconds` steps.
/// `dt` is added only when it is positive and finite.
/// A non-positive or non-finite `tickSeconds` returns 0 and leaves `accumulator` unchanged.
/// `maxTicks` below 1 returns 0 after that add, without consuming a tick.
/// At most `maxTicks` whole ticks are consumed. The leftover stays in `accumulator`.
[[nodiscard]] inline int accumulate_ticks(float& accumulator, float dt, float tickSeconds,
                                          int maxTicks) {
    if (!std::isfinite(tickSeconds) || !(tickSeconds > 0.f)) {
        return 0;
    }
    if (std::isfinite(dt) && dt > 0.f) {
        accumulator += dt;
    }
    if (maxTicks < 1) {
        return 0;
    }

    int ticks = 0;
    while (ticks < maxTicks && accumulator >= tickSeconds) {
        accumulator -= tickSeconds;
        ++ticks;
    }
    return ticks;
}

} // namespace fuse::editor
