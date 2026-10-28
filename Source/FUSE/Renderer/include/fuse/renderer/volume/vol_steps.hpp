#pragma once

namespace fuse::renderer {

/// True when `steps` is a power of two in the closed interval [8, 128].
[[nodiscard]] inline bool vol_steps_ok(int steps) {
    if (steps < 8 || steps > 128) {
        return false;
    }
    return (steps & (steps - 1)) == 0;
}

} // namespace fuse::renderer
