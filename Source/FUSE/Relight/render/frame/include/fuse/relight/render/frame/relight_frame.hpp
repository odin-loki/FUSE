#pragma once

#include <cstdint>

namespace fuse::relight {

/// True when `index` is in the inclusive range 0..1000000.
[[nodiscard]] inline bool relight_frame_ok(std::uint32_t index) {
    return index <= 1000000u;
}

} // namespace fuse::relight
