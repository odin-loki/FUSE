#pragma once

#include <fuse/math/vec.hpp>

#include <cmath>

namespace fuse::renderer {

enum class ColorVision : u8 {
    Typical = 0,
    Protanopia = 1,
    Deuteranopia = 2,
    Tritanopia = 3,
};

/// Viénot-style linear-RGB matrices (row-major). Output channels are clamped to 0..1.
/// Protanopia:
///   0.152286  1.052583 -0.204868
///   0.114503  0.786281  0.099216
///  -0.003882 -0.048116  1.051998
/// Deuteranopia:
///   0.367322  0.860646 -0.227968
///   0.280085  0.672501  0.047413
///  -0.011820  0.042940  0.968881
/// Tritanopia:
///   1.255528 -0.076749 -0.178779
///  -0.078411  0.930809  0.147602
///   0.004733  0.691367  0.303900
[[nodiscard]] inline math::Vec3 simulate_color_vision(math::Vec3 linearRgb, ColorVision kind) {
    if (!std::isfinite(linearRgb.x) || !std::isfinite(linearRgb.y) || !std::isfinite(linearRgb.z)) {
        return {};
    }
    const f32* m = nullptr;
    static constexpr f32 kProtanopia[9] = {0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f,
                                           0.099216f, -0.003882f, -0.048116f, 1.051998f};
    static constexpr f32 kDeuteranopia[9] = {0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f,
                                             0.047413f, -0.011820f, 0.042940f, 0.968881f};
    static constexpr f32 kTritanopia[9] = {1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f,
                                           0.147602f, 0.004733f, 0.691367f, 0.303900f};
    switch (kind) {
    case ColorVision::Protanopia:
        m = kProtanopia;
        break;
    case ColorVision::Deuteranopia:
        m = kDeuteranopia;
        break;
    case ColorVision::Tritanopia:
        m = kTritanopia;
        break;
    case ColorVision::Typical:
        break;
    }
    auto clamp01 = [](f32 v) {
        if (v < 0.f) {
            return 0.f;
        }
        if (v > 1.f) {
            return 1.f;
        }
        return v;
    };
    if (m == nullptr) {
        return {clamp01(linearRgb.x), clamp01(linearRgb.y), clamp01(linearRgb.z)};
    }
    return {clamp01(m[0] * linearRgb.x + m[1] * linearRgb.y + m[2] * linearRgb.z),
            clamp01(m[3] * linearRgb.x + m[4] * linearRgb.y + m[5] * linearRgb.z),
            clamp01(m[6] * linearRgb.x + m[7] * linearRgb.y + m[8] * linearRgb.z)};
}

} // namespace fuse::renderer
