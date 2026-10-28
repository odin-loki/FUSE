#pragma once

namespace fuse::renderer {

/// True only for frames 0 through 7 inclusive.
[[nodiscard]] inline bool temporal_frame_ok(int frame) {
    return frame >= 0 && frame <= 7;
}

} // namespace fuse::renderer
