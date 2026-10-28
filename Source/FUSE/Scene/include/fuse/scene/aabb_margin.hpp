#pragma once

#include <cmath>
#include <utility>

namespace fuse::scene {

struct SceneAabb {
    float minX = 0.f;
    float minY = 0.f;
    float minZ = 0.f;
    float maxX = 0.f;
    float maxY = 0.f;
    float maxZ = 0.f;
};

inline SceneAabb expand_aabb(SceneAabb box, float margin) {
    const bool expand = std::isfinite(margin) && margin > 0.f;
    auto axis = [&](float& lo, float& hi) {
        if (!std::isfinite(lo)) {
            lo = 0.f;
        }
        if (!std::isfinite(hi)) {
            hi = 0.f;
        }
        if (lo > hi) {
            std::swap(lo, hi);
        }
        if (expand) {
            lo -= margin;
            hi += margin;
        }
    };
    axis(box.minX, box.maxX);
    axis(box.minY, box.maxY);
    axis(box.minZ, box.maxZ);
    return box;
}

} // namespace fuse::scene
