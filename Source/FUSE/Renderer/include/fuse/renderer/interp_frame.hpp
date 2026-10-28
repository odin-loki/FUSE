#pragma once

namespace fuse::renderer {

/// True only for frames 0 through 3 inclusive.
[[nodiscard]] inline bool interp_frame_ok(int frame) {
    return frame >= 0 && frame <= 3;
}

} // namespace fuse::renderer
