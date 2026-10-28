#pragma once

#include <cstdint>

namespace fuse::relight {

/// True when \p frame is in 0..10000.
[[nodiscard]] inline bool capture_frame_ok(std::uint32_t frame) {
    return frame <= 10000u;
}

} // namespace fuse::relight
