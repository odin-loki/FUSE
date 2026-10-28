#pragma once

namespace fuse::renderer {

/// True only for frames 0 through 1000000 inclusive.
[[nodiscard]] inline bool rp_frame_ok(int frame) {
    return frame >= 0 && frame <= 1000000;
}

} // namespace fuse::renderer
