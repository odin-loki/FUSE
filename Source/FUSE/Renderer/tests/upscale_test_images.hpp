#pragma once

// Test-only helpers for the upscaler gates (test_upscale_gates.cpp, test_upscale_gpu_reference.cpp):
// a deterministic analytic test scene rendered with box-filter anti-aliasing at any resolution (optionally
// with a sub-pixel jitter), and a hash noise image. Metrics come from fuse::renderer::quality
// (quality/image_metrics.hpp). The render-resolution input and the display-resolution ground truth are the same continuous
// scene, each area-averaged over its own pixel footprint, so "upscaled vs ground truth" measures how well a
// backend reconstructs detail the render resolution lost.

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fuse::upscale_test {

using math::Vec4;

/// Analytic scene at normalized coordinates (u, v) in [0, 1)^2, colour in [0, 1] (tone-mapped, as FSR1 /
/// NIS / CAS expect). Contains smooth gradients, hard edges at many angles, sub-pixel lines, a Siemens
/// star (all frequencies), a fine checker patch, and pure black / white areas (RCAS / CAS limit paths).
inline Vec4 scene(f32 u, f32 v) {
    // Background: smooth two-axis gradient.
    f32 r = 0.20f + 0.45f * u;
    f32 g = 0.25f + 0.35f * v;
    f32 b = 0.55f - 0.30f * u * v;

    // Disk (hard edge, all angles).
    const f32 du = u - 0.28f;
    const f32 dv = v - 0.33f;
    if (du * du + dv * dv < 0.17f * 0.17f) {
        r = 0.92f;
        g = 0.34f;
        b = 0.18f;
    }
    // Rotated square outline (20 degrees).
    {
        const f32 c = std::cos(0.349f);
        const f32 s = std::sin(0.349f);
        const f32 x = (u - 0.30f) * c + (v - 0.78f) * s;
        const f32 y = -(u - 0.30f) * s + (v - 0.78f) * c;
        const f32 m = std::max(std::fabs(x), std::fabs(y));
        if (m < 0.14f && m > 0.105f) {
            r = 0.08f;
            g = 0.55f;
            b = 0.30f;
        }
    }
    // Thin dark lines at several angles (sub-pixel at render resolution).
    const f32 angles[4] = {0.17f, 0.61f, 1.05f, 1.40f};
    for (u32 i = 0; i < 4u; ++i) {
        const f32 nx = std::cos(angles[i]);
        const f32 ny = std::sin(angles[i]);
        const f32 d = std::fabs((u - 0.55f) * nx + (v - 0.12f - 0.06f * static_cast<f32>(i)) * ny);
        if (d < 0.0022f && u > 0.48f && u < 0.98f) {
            r = 0.05f;
            g = 0.05f;
            b = 0.07f;
        }
    }
    // Siemens star.
    {
        const f32 x = u - 0.74f;
        const f32 y = v - 0.64f;
        const f32 rr = std::sqrt(x * x + y * y);
        if (rr < 0.20f) {
            const f32 a = std::atan2(y, x);
            const bool spoke = std::sin(a * 18.f) > 0.f;
            r = g = b = spoke ? 0.88f : 0.12f;
        }
    }
    // Fine checker patch.
    if (u > 0.05f && u < 0.20f && v > 0.55f && v < 0.64f) {
        const s32 cx = static_cast<s32>(std::floor(u * 180.f));
        const s32 cy = static_cast<s32>(std::floor(v * 180.f));
        const bool on = ((cx + cy) & 1) != 0;
        r = g = b = on ? 0.8f : 0.25f;
    }
    // Pure black and pure white bars.
    if (v > 0.93f && u < 0.30f) {
        r = g = b = 0.f;
    }
    if (v > 0.93f && u > 0.70f) {
        r = g = b = 1.f;
    }
    return Vec4(r, g, b, 1.f);
}

/// Scene rendered at `width x height` with an `ss x ss` box filter per pixel, every pixel footprint shifted
/// by (`jitter_x`, `jitter_y`) pixels (the TAAU convention: pixel i samples around i + 0.5 + jitter).
inline std::vector<Vec4> render_scene(u32 width, u32 height, u32 ss = 6u, f32 jitter_x = 0.f, f32 jitter_y = 0.f) {
    std::vector<Vec4> image(static_cast<usize>(width) * height);
    const f32 inv = 1.f / static_cast<f32>(ss * ss);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            f32 r = 0.f;
            f32 g = 0.f;
            f32 b = 0.f;
            for (u32 sy = 0; sy < ss; ++sy) {
                for (u32 sx = 0; sx < ss; ++sx) {
                    const f32 u =
                        (static_cast<f32>(x) + jitter_x + (static_cast<f32>(sx) + 0.5f) / static_cast<f32>(ss)) /
                        static_cast<f32>(width);
                    const f32 v =
                        (static_cast<f32>(y) + jitter_y + (static_cast<f32>(sy) + 0.5f) / static_cast<f32>(ss)) /
                        static_cast<f32>(height);
                    const Vec4 c = scene(u, v);
                    r += c.x;
                    g += c.y;
                    b += c.z;
                }
            }
            image[static_cast<usize>(y) * width + x] = Vec4(r * inv, g * inv, b * inv, 1.f);
        }
    }
    return image;
}

/// Deterministic pseudo-random image in [lo, hi] (hash-based; bit-identical everywhere).
inline std::vector<Vec4> noise_image(u32 width, u32 height, u32 seed, f32 lo = 0.f, f32 hi = 1.f) {
    std::vector<Vec4> image(static_cast<usize>(width) * height);
    u32 state = seed * 747796405u + 2891336453u;
    const auto next = [&state, lo, hi] {
        state = state * 747796405u + 2891336453u;
        u32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        word = (word >> 22u) ^ word;
        return lo + (hi - lo) * static_cast<f32>(word >> 8u) / static_cast<f32>(1u << 24u);
    };
    for (Vec4& c : image) {
        c.x = next();
        c.y = next();
        c.z = next();
        c.w = 1.f;
    }
    return image;
}

/// RGB copy (the quality metrics take Vec3 images).
inline std::vector<math::Vec3> to_rgb(const std::vector<Vec4>& image) {
    std::vector<math::Vec3> out(image.size());
    for (usize i = 0; i < image.size(); ++i) {
        out[i] = math::Vec3(image[i].x, image[i].y, image[i].z);
    }
    return out;
}

} // namespace fuse::upscale_test
