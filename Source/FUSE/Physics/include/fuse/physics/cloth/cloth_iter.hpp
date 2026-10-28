#pragma once

namespace fuse::physics {

/// True when a cloth iteration count is in the inclusive range 1..32.
[[nodiscard]] inline bool cloth_iter_ok(int iterations) {
    return iterations >= 1 && iterations <= 32;
}

} // namespace fuse::physics
