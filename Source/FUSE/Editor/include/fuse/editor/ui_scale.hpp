#pragma once

#include <cmath>

namespace fuse::editor {

inline float snap_ui_scale(float scale) {
    if (!std::isfinite(scale) || scale <= 0.f) {
        return 1.f;
    }
    if (scale < 1.f) {
        scale = 1.f;
    }
    if (scale > 2.f) {
        scale = 2.f;
    }
    const float options[] = {1.f, 1.25f, 1.5f, 2.f};
    float best = options[0];
    float bestDistance = std::fabs(scale - best);
    for (int i = 1; i < 4; ++i) {
        const float distance = std::fabs(scale - options[i]);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = options[i];
        }
    }
    return best;
}

} // namespace fuse::editor
