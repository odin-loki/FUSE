#pragma once

#include <fuse/types.hpp>

#include <cmath>
#include <span>

namespace fuse::asset {

struct MeshLod {
    f32 geometricError = 0.f;
};

/// Screen-space error = (geometricError / distance) * (screenHeightPx * 0.5) / tan(fovY/2) * lodBias.
/// Returns the coarsest lod whose error is still <= 1 pixel.
[[nodiscard]] inline u32 select_mesh_lod(std::span<const MeshLod> lods, f32 distance, f32 screenHeightPx,
                                         f32 fovYRadians, f32 lodBias) {
    if (lods.empty() || !std::isfinite(distance) || distance <= 0.f || !std::isfinite(screenHeightPx) ||
        screenHeightPx <= 0.f || !std::isfinite(fovYRadians) || fovYRadians <= 0.f ||
        fovYRadians >= 3.14159265f) {
        return 0u;
    }
    const f32 bias = (!std::isfinite(lodBias) || lodBias <= 0.f) ? 1.f : lodBias;
    const f32 tanHalf = std::tan(fovYRadians * 0.5f);
    if (!std::isfinite(tanHalf) || tanHalf <= 0.f) {
        return 0u;
    }
    const f32 pixelsPerRadian = (screenHeightPx * 0.5f) / tanHalf;
    f32 previous = 0.f;
    u32 chosen = 0u;
    bool any = false;
    for (u32 i = 0; i < static_cast<u32>(lods.size()); ++i) {
        const f32 error = lods[i].geometricError;
        if (!std::isfinite(error) || (i > 0u && !(error > previous))) {
            return 0u;
        }
        previous = error;
        const f32 screenError = (error / distance) * pixelsPerRadian * bias;
        if (screenError <= 1.f) {
            chosen = i;
            any = true;
        }
    }
    return any ? chosen : 0u;
}

} // namespace fuse::asset
