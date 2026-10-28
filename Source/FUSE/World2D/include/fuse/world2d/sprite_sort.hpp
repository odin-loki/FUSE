#pragma once

#include <algorithm>

namespace fuse::world2d {

/// Painter key. A higher layer sorts after a lower layer when `y` is equal.
/// `layer` is clamped to [-1000, 1000] and `y` to [-100000, 100000].
[[nodiscard]] inline int sprite_sort_key(int layer, int y) {
    constexpr int kLayerMin = -1000;
    constexpr int kLayerMax = 1000;
    constexpr int kYMin = -100000;
    constexpr int kYMax = 100000;
    constexpr int kLayerStride = 100000;
    const int clampedLayer = std::clamp(layer, kLayerMin, kLayerMax);
    const int clampedY = std::clamp(y, kYMin, kYMax);
    return clampedLayer * kLayerStride + clampedY;
}

} // namespace fuse::world2d
