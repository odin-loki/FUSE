#pragma once

namespace fuse::physics {

/// True only for steps 1 through 16 inclusive.
[[nodiscard]] inline bool rotation_steps_ok(int steps) {
    return steps >= 1 && steps <= 16;
}

} // namespace fuse::physics
