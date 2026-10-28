// Path bounce count. A count is valid only when it lies in [1, 16].
#pragma once

namespace fuse::relight {

/// True when `bounces` lies in the inclusive range 1..16.
[[nodiscard]] inline bool pt_bounce_ok(int bounces) {
    return bounces >= 1 && bounces <= 16;
}

} // namespace fuse::relight
