#pragma once

#include <fuse/math/vec.hpp>

#include <cmath>

namespace fuse::renderer {

/// result = center + (center - blurred) * amount. Negative amount is treated as zero.
[[nodiscard]] inline math::Vec3 unsharp_mask(math::Vec3 center, math::Vec3 blurred, f32 amount) {
    auto finite = [](math::Vec3 v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    if (!finite(center)) {
        return {};
    }
    if (!std::isfinite(amount) || amount <= 0.f || !finite(blurred)) {
        return center;
    }
    const math::Vec3 delta{center.x - blurred.x, center.y - blurred.y, center.z - blurred.z};
    return {center.x + delta.x * amount, center.y + delta.y * amount, center.z + delta.z * amount};
}

} // namespace fuse::renderer
