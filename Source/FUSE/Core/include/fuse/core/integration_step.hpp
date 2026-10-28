#pragma once

namespace fuse::core {

/// True when `step` is in the closed range [0, 8].
[[nodiscard]] inline bool integration_step_ok(int step) {
    return step >= 0 && step <= 8;
}

} // namespace fuse::core
