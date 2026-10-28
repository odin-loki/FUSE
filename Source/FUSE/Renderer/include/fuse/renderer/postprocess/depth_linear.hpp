#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::renderer {

/// View-space distance in meters from a reversed-Z depth. depth01 is 1 at the near plane and 0 at
/// the far plane. The mapping is the reciprocal-space lerp 1 / (depth/near + (1-depth)/far).
/// Depth is clamped to 0..1 first. A non-positive near plane, a far plane that is not beyond near,
/// or a non-finite plane returns 0.
[[nodiscard]] inline f32 linearize_reverse_z(f32 depth01, f32 nearM, f32 farM) {
    if (std::isnan(depth01) || depth01 < 0.f) {
        depth01 = 0.f;
    } else if (depth01 > 1.f) {
        depth01 = 1.f;
    }
    if (!std::isfinite(nearM) || !std::isfinite(farM) || nearM <= 0.f || farM <= nearM) {
        return 0.f;
    }
    return 1.f / (depth01 / nearM + (1.f - depth01) / farM);
}

} // namespace fuse::renderer
