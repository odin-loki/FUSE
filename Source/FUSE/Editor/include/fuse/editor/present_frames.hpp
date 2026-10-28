#pragma once

namespace fuse::editor {

/// True when `frames` is in the inclusive range 1..4.
[[nodiscard]] inline bool present_frames_ok(int frames) {
    return frames >= 1 && frames <= 4;
}

} // namespace fuse::editor
