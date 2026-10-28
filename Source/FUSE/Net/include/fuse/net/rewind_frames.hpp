#pragma once

namespace fuse::net {

/// True only for frame counts 0 through 32 inclusive.
[[nodiscard]] inline bool rewind_frames_ok(int frames) {
    return frames >= 0 && frames <= 32;
}

} // namespace fuse::net
