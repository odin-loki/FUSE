#pragma once

namespace fuse::renderer {

/// True only when `steps` is a power of two in the closed interval [4, 64]. Zero is false.
[[nodiscard]] inline bool atmosphere_steps_ok(int steps) {
    if (steps < 4 || steps > 64) {
        return false;
    }
    return (steps & (steps - 1)) == 0;
}

} // namespace fuse::renderer
