#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::renderer::ssfx {

/// 1 at reprojection error 0, 0 at and beyond `maxError`, linear between.
/// `maxError` <= 0 returns 0. Non-finite inputs return 0. A negative error is treated as 0.
[[nodiscard]] inline f32 history_confidence(f32 reprojectionError, f32 maxError) {
    if (!std::isfinite(reprojectionError) || !std::isfinite(maxError) || maxError <= 0.f) {
        return 0.f;
    }
    const f32 error = reprojectionError < 0.f ? 0.f : reprojectionError;
    if (error >= maxError) {
        return 0.f;
    }
    return 1.f - error / maxError;
}

} // namespace fuse::renderer::ssfx
