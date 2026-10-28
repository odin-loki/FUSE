#pragma once

namespace fuse::renderer {

/// True when `frames` is in the inclusive range 0..8.
[[nodiscard]] inline bool latency_frames_ok(int frames) {
    return frames >= 0 && frames <= 8;
}

} // namespace fuse::renderer
