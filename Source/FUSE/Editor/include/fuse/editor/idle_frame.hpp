#pragma once

#include <cmath>

namespace fuse::editor {

/// True when no input has arrived for at least `idleAfterSeconds`.
/// Both arguments must be finite and `idleAfterSeconds` must be greater than 0.
/// A negative threshold, zero, or any non-finite value is not idle.
[[nodiscard]] inline bool editor_idle(float secondsSinceInput, float idleAfterSeconds) {
    if (!std::isfinite(secondsSinceInput) || !std::isfinite(idleAfterSeconds)) {
        return false;
    }
    if (!(idleAfterSeconds > 0.f)) {
        return false;
    }
    return secondsSinceInput >= idleAfterSeconds;
}

/// Delta used for an editor frame.
/// While idle, `idleScale` is saturated into [0, 1] (non-finite becomes 0) and the
/// result is `dt` times that scale. Otherwise a finite `dt` greater than 0 is returned
/// unchanged, and every other `dt` becomes 0.
[[nodiscard]] inline float idle_dt(float dt, bool idle, float idleScale) {
    if (idle) {
        float scale = idleScale;
        if (!std::isfinite(scale) || scale < 0.f) {
            scale = 0.f;
        } else if (scale > 1.f) {
            scale = 1.f;
        }
        return dt * scale;
    }
    if (std::isfinite(dt) && dt > 0.f) {
        return dt;
    }
    return 0.f;
}

} // namespace fuse::editor
