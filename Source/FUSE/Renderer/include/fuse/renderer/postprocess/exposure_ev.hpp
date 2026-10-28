#pragma once

#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

/// Linear exposure multiplier for an EV bias: pow(2, ev).
/// A non-finite ev returns 1. ev is clamped to [-16, 16] before the power so the result stays finite.
[[nodiscard]] inline f32 exposure_from_ev(f32 ev) {
    if (!std::isfinite(ev)) {
        return 1.f;
    }
    return std::pow(2.f, std::clamp(ev, -16.f, 16.f));
}

/// EV bias of a linear exposure multiplier: log2(exposure), then clamped to [-16, 16].
/// A non-positive or non-finite exposure returns 0.
[[nodiscard]] inline f32 ev_from_exposure(f32 exposure) {
    if (!std::isfinite(exposure) || exposure <= 0.f) {
        return 0.f;
    }
    return std::clamp(std::log2(exposure), -16.f, 16.f);
}

} // namespace fuse::renderer
