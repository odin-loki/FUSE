#pragma once

#include <cmath>

namespace fuse::editor {

/// Caps a play-mode frame delta. Non-finite or negative dt returns 0.
/// Otherwise the value is clamped to at most 0.1 seconds.
[[nodiscard]] inline float cap_play_dt(float dt) {
    if (!std::isfinite(dt) || dt < 0.f) {
        return 0.f;
    }
    if (dt > 0.1f) {
        return 0.1f;
    }
    return dt;
}

} // namespace fuse::editor
