#pragma once

namespace fuse::world2d {

/// Supported World2D frame rates are 30 and 60.
[[nodiscard]] inline bool world2d_fps_ok(int fps) {
    return fps == 30 || fps == 60;
}

} // namespace fuse::world2d
