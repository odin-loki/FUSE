#pragma once

namespace fuse::renderer {

/// True only for modes 0 through 3 inclusive.
[[nodiscard]] inline bool present_mode_ok(int mode) {
    return mode >= 0 && mode <= 3;
}

} // namespace fuse::renderer
