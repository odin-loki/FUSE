#pragma once

#include <algorithm>
#include <cmath>

namespace fuse::relight::render::material {

/// Separable Smith GGX geometry term (height correlation is not applied):
/// G = G1(nDotV) * G1(nDotL),
/// G1(x) = 2x / (x + sqrt(alpha^2 + (1 - alpha^2) * x * x)).
/// Dots are clamped to [0, 1] and alpha to [1e-3, 1]. A non-finite input or result returns 0.
[[nodiscard]] inline float smith_ggx_g(float nDotV, float nDotL, float alpha) {
    if (!std::isfinite(nDotV) || !std::isfinite(nDotL) || !std::isfinite(alpha)) {
        return 0.f;
    }
    const float a = std::clamp(alpha, 1e-3f, 1.f);
    const float a2 = a * a;
    const auto g1 = [a2](float x) {
        const float c = std::clamp(x, 0.f, 1.f);
        const float root = std::sqrt(a2 + (1.f - a2) * c * c);
        return (2.f * c) / (c + root);
    };
    const float g = g1(nDotV) * g1(nDotL);
    return std::isfinite(g) ? g : 0.f;
}

} // namespace fuse::relight::render::material
