#pragma once

namespace fuse::scene {

/// True when `steps` is a power of two in the closed interval [16, 256].
[[nodiscard]] inline bool march_budget_ok(int steps) {
    if (steps < 16 || steps > 256) {
        return false;
    }
    return (steps & (steps - 1)) == 0;
}

} // namespace fuse::scene
