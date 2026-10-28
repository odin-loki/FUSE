#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `ms` is finite and in the inclusive range 0..100.
[[nodiscard]] inline bool frame_ms_ok(float ms) {
    return std::isfinite(ms) && ms >= 0.f && ms <= 100.f;
}

} // namespace fuse::renderer
