#pragma once

#include <cmath>

namespace fuse::renderer {

inline float max_channel_error(float aR, float aG, float aB, float bR, float bG, float bB) {
    if (!std::isfinite(aR) || !std::isfinite(aG) || !std::isfinite(aB) || !std::isfinite(bR) || !std::isfinite(bG) ||
        !std::isfinite(bB)) {
        return 1.0e9f;
    }
    const float dr = std::fabs(aR - bR);
    const float dg = std::fabs(aG - bG);
    const float db = std::fabs(aB - bB);
    float worst = dr;
    if (dg > worst) {
        worst = dg;
    }
    if (db > worst) {
        worst = db;
    }
    return worst;
}

inline bool within_error(float error, float tolerance) {
    return std::isfinite(error) && std::isfinite(tolerance) && tolerance >= 0.f && error <= tolerance;
}

} // namespace fuse::renderer
