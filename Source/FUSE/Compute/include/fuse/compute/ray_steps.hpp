#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::compute {

/// Steps to cover `distance` at length `step`: ceil(distance / step), at least 1.
/// Non-positive or non-finite inputs return 0. The count saturates at 4096.
[[nodiscard]] inline u32 ray_step_count(f32 distance, f32 step) {
    if (!(distance > 0.f) || !(step > 0.f) || !std::isfinite(distance) || !std::isfinite(step)) {
        return 0u;
    }
    const f32 ratio = distance / step;
    if (!std::isfinite(ratio) || ratio > 4096.f) {
        return 4096u;
    }
    const f32 steps = std::ceil(ratio);
    if (!(steps >= 1.f)) {
        return 1u;
    }
    return static_cast<u32>(steps);
}

} // namespace fuse::compute
