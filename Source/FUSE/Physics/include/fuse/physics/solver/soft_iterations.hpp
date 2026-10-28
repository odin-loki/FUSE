#pragma once

namespace fuse::physics {

[[nodiscard]] inline bool soft_iterations_ok(int count) {
    return count >= 1 && count <= 32;
}

} // namespace fuse::physics
