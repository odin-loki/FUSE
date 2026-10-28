#pragma once

// Neighborhood AABB clip for temporal denoise (CPU). The denoise shader is unchanged.

#include <cmath>

namespace fuse::relight::render::denoise {

struct Rgb {
    float r, g, b;
};

/// Clamp each channel of `color` into `[lo, hi]`, swapping that channel's bounds when `lo > hi`.
/// A non-finite color channel becomes the finite `lo`, or 0 when `lo` is also non-finite.
[[nodiscard]] inline Rgb clip_color_aabb(Rgb color, Rgb lo, Rgb hi) {
    const auto clipChannel = [](float c, float a, float b) {
        if (!std::isfinite(c)) {
            return std::isfinite(a) ? a : 0.f;
        }
        if (a > b) {
            const float swapped = a;
            a = b;
            b = swapped;
        }
        if (c < a) {
            c = a;
        }
        if (c > b) {
            c = b;
        }
        return c;
    };
    color.r = clipChannel(color.r, lo.r, hi.r);
    color.g = clipChannel(color.g, lo.g, hi.g);
    color.b = clipChannel(color.b, lo.b, hi.b);
    return color;
}

} // namespace fuse::relight::render::denoise
