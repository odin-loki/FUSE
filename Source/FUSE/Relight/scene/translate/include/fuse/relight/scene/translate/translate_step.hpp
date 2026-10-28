#pragma once

namespace fuse::relight {

/// True when \p step is in the closed range 0..16.
[[nodiscard]] inline bool translate_step_ok(int step) {
    return step >= 0 && step <= 16;
}

} // namespace fuse::relight
