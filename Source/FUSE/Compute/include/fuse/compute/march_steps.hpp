#pragma once

namespace fuse::compute {

/// True when `steps` is a power of two in the closed interval [16, 256].
[[nodiscard]] inline bool march_steps_ok(int steps) {
    return steps > 0 && (steps & (steps - 1)) == 0 && steps >= 16 && steps <= 256;
}

} // namespace fuse::compute
