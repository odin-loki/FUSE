#pragma once

#include <cmath>

namespace fuse::relight::render::denoise {

inline float variance_gate(float variance, float sigma) {
    if (!std::isfinite(variance) || !std::isfinite(sigma) || !(sigma > 0.f)) {
        return 0.f;
    }
    if (variance < 0.f) {
        variance = 0.f;
    }
    float weight = 1.f - variance / sigma;
    if (weight < 0.f) {
        weight = 0.f;
    }
    if (weight > 1.f) {
        weight = 1.f;
    }
    return weight;
}

} // namespace fuse::relight::render::denoise
