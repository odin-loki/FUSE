#pragma once

#include <cmath>

namespace fuse::hybrid {

/// True when the closed axis-aligned rects overlap. Touching edges overlap.
/// A non-positive or non-finite width or height means no overlap.
[[nodiscard]] inline bool rects_overlap(float ax, float ay, float aw, float ah, float bx, float by, float bw,
                                        float bh) {
    if (!std::isfinite(aw) || !std::isfinite(ah) || !std::isfinite(bw) || !std::isfinite(bh) || !(aw > 0.f) ||
        !(ah > 0.f) || !(bw > 0.f) || !(bh > 0.f)) {
        return false;
    }
    return ax <= bx + bw && bx <= ax + aw && ay <= by + bh && by <= ay + ah;
}

} // namespace fuse::hybrid
