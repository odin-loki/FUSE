#pragma once

namespace fuse::project {

/// True for cook steps 0 through 8 inclusive.
[[nodiscard]] inline bool cook_step_ok(int step) {
    return step >= 0 && step <= 8;
}

} // namespace fuse::project
