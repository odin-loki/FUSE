#pragma once

namespace fuse::renderer {

/// True when `step` is in the inclusive range 0..8.
[[nodiscard]] inline bool deferred_step_ok(int step) {
    return step >= 0 && step <= 8;
}

} // namespace fuse::renderer
