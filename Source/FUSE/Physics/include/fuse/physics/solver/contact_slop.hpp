#pragma once

#include <fuse/physics/types.hpp>

#include <cmath>

namespace fuse::physics {

/// Excess penetration past the allowed overlap.
/// Non-finite inputs return 0. A negative allowed overlap counts as 0.
/// Penetration at or below the allowed overlap returns 0; otherwise the excess.
[[nodiscard]] inline f32 contact_slop(f32 penetration, f32 allowed) {
    if (!std::isfinite(penetration) || !std::isfinite(allowed)) {
        return 0.f;
    }
    if (allowed < 0.f) {
        allowed = 0.f;
    }
    if (penetration <= allowed) {
        return 0.f;
    }
    return penetration - allowed;
}

} // namespace fuse::physics
