#pragma once

namespace fuse::core {

/// True when `frames` is in the closed range [1, 120].
[[nodiscard]] inline bool smoke_frame_ok(int frames) {
    return frames >= 1 && frames <= 120;
}

} // namespace fuse::core
