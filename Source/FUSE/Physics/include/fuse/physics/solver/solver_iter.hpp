#pragma once

namespace fuse::physics {

/// True when a solver iteration count is in the inclusive range 1..64.
[[nodiscard]] inline bool solver_iter_ok(int iterations) {
    return iterations >= 1 && iterations <= 64;
}

} // namespace fuse::physics
