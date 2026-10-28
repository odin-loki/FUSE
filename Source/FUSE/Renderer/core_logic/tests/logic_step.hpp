#pragma once

namespace fuse::renderer {

/// True only for steps 0 through 8 inclusive.
[[nodiscard]] inline bool logic_step_ok(int step) {
    return step >= 0 && step <= 8;
}

} // namespace fuse::renderer
