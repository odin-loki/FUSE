#pragma once

namespace fuse::physics {

/// True when a solver iteration count is in the inclusive range 1..64.
[[nodiscard]] inline bool solver_iterations_ok(int count) {
    return count >= 1 && count <= 64;
}

} // namespace fuse::physics
