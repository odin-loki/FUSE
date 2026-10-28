#pragma once

namespace fuse::relight {

/// True when \p frame is in the closed range [1, 120].
[[nodiscard]] inline bool roundtrip_frame_ok(int frame) {
    return frame >= 1 && frame <= 120;
}

} // namespace fuse::relight
