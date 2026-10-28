#pragma once

#include <cstdint>

namespace fuse::renderer {

/// True when `frame` is in the inclusive range 0..1000000.
[[nodiscard]] inline bool renderer_frame_ok(std::uint32_t frame) {
    return frame <= 1000000u;
}

} // namespace fuse::renderer
