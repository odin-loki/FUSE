#pragma once

#include <cmath>

namespace fuse::vfx {

/// True when `seconds` is finite and in the inclusive range 0..30.
[[nodiscard]] inline bool particle_life_ok(float seconds) {
    return std::isfinite(seconds) && seconds >= 0.f && seconds <= 30.f;
}

} // namespace fuse::vfx
