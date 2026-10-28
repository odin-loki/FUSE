#pragma once

#include <cstdint>

namespace fuse::hybrid {

/// True when `frame` is in the inclusive range 0..1000000.
[[nodiscard]] inline bool hybrid_frame_ok(std::uint32_t frame) {
    return frame <= 1000000u;
}

} // namespace fuse::hybrid
