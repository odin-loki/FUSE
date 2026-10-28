#pragma once

#include <cmath>

namespace fuse::renderer {

inline float snap_shadow_texel(float lightSpace, float orthoSize, int resolution) {
    if (!std::isfinite(lightSpace)) {
        return 0.f;
    }
    if (resolution <= 0 || !(orthoSize > 0.f) || !std::isfinite(orthoSize)) {
        return lightSpace;
    }
    float u = lightSpace / orthoSize + 0.5f;
    if (u < 0.f) {
        u = 0.f;
    }
    int index = static_cast<int>(std::floor(u * static_cast<float>(resolution)));
    if (index < 0) {
        index = 0;
    }
    if (index >= resolution) {
        index = resolution - 1;
    }
    return (static_cast<float>(index) + 0.5f) / static_cast<float>(resolution) * orthoSize - orthoSize * 0.5f;
}

} // namespace fuse::renderer
