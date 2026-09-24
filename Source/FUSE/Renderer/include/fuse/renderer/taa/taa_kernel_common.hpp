#pragma once

// Device-safe temporal-AA building blocks shared by the same-resolution CPU TAA (`taa_cpu_resolve.cpp`)
// and the single-source TAAU kernel (`taau_kernel.hpp`). Every helper is FUSE_HOST_DEVICE and only uses
// fuse/math + <cmath> float functions, so the same code runs on the CPU backends and in a CUDA trampoline
// (docs/compute-kernels.md). This is the ONLY implementation of these helpers: the public `taa*` wrappers in
// taa_cpu_resolve.hpp forward here.

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::taa_common {

FUSE_HOST_DEVICE inline f32 saturate(f32 v) { return std::max(0.f, std::min(1.f, v)); }

FUSE_HOST_DEVICE inline math::Vec3 lerp3(const math::Vec3& a, const math::Vec3& b, f32 t) { return a + (b - a) * t; }

FUSE_HOST_DEVICE inline math::Vec3 min3(const math::Vec3& a, const math::Vec3& b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

FUSE_HOST_DEVICE inline math::Vec3 max3(const math::Vec3& a, const math::Vec3& b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

FUSE_HOST_DEVICE inline math::Vec3 mul3(const math::Vec3& a, const math::Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

FUSE_HOST_DEVICE inline i32 clamp_index(i32 v, i32 hi) { return v < 0 ? 0 : (v > hi ? hi : v); }

/// Edge-clamped texel fetch from a row-major image (any element type).
template <typename T>
FUSE_HOST_DEVICE inline const T& texel(const T* image, u32 width, u32 height, i32 x, i32 y) {
    const i32 cx = clamp_index(x, static_cast<i32>(width) - 1);
    const i32 cy = clamp_index(y, static_cast<i32>(height) - 1);
    return image[static_cast<u32>(cy) * width + static_cast<u32>(cx)];
}

/// Rec. 709 luma of linear RGB.
FUSE_HOST_DEVICE inline f32 luma709(const math::Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

/// RGB -> YCoCg (x = Y, y = Co, z = Cg).
FUSE_HOST_DEVICE inline math::Vec3 rgb_to_ycocg(const math::Vec3& rgb) {
    return {0.25f * rgb.x + 0.5f * rgb.y + 0.25f * rgb.z, 0.5f * rgb.x - 0.5f * rgb.z,
            -0.25f * rgb.x + 0.5f * rgb.y - 0.25f * rgb.z};
}

FUSE_HOST_DEVICE inline math::Vec3 ycocg_to_rgb(const math::Vec3& c) {
    const f32 tmp = c.x - c.z;
    return {tmp + c.y, c.x + c.z, tmp - c.y};
}

/// Clip `history` towards the AABB centre so it lies inside the box (Karis 2014 / Playdead INSIDE TAA).
FUSE_HOST_DEVICE inline math::Vec3 clip_to_aabb(const math::Vec3& history, const math::Vec3& boxMin,
                                                const math::Vec3& boxMax) {
    const math::Vec3 center = (boxMin + boxMax) * 0.5f;
    const math::Vec3 extent = (boxMax - boxMin) * 0.5f;
    const math::Vec3 offset = history - center;
    const f32 eps = 1e-6f;
    const f32 ux = std::fabs(offset.x) / std::max(extent.x, eps);
    const f32 uy = std::fabs(offset.y) / std::max(extent.y, eps);
    const f32 uz = std::fabs(offset.z) / std::max(extent.z, eps);
    const f32 maxUnit = std::max(ux, std::max(uy, uz));
    if (maxUnit > 1.f) {
        return center + offset * (1.f / maxUnit);
    }
    return history;
}

/// Catmull-Rom cubic weights for fractional position `t` in [0, 1) (taps at -1, 0, +1, +2).
FUSE_HOST_DEVICE inline void catmull_rom_weights(f32 t, f32 w[4]) {
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    w[0] = 0.5f * (-t3 + 2.f * t2 - t);
    w[1] = 0.5f * (3.f * t3 - 5.f * t2 + 2.f);
    w[2] = 0.5f * (-3.f * t3 + 4.f * t2 + t);
    w[3] = 0.5f * (t3 - t2);
}

/// sinc(x) * sinc(x / 2) for |x| < 2, else 0 (Lanczos-2 window).
FUSE_HOST_DEVICE inline f32 lanczos2(f32 x) {
    const f32 ax = std::fabs(x);
    if (ax < 1e-5f) {
        return 1.f;
    }
    if (ax >= 2.f) {
        return 0.f;
    }
    constexpr f32 kPi = 3.14159265358979f;
    const f32 px = kPi * ax;
    return 2.f * std::sin(px) * std::sin(0.5f * px) / (px * px);
}

/// Normalised Lanczos-2 weights for fractional position `t` (taps at -1, 0, +1, +2).
FUSE_HOST_DEVICE inline void lanczos2_weights(f32 t, f32 w[4]) {
    w[0] = lanczos2(t + 1.f);
    w[1] = lanczos2(t);
    w[2] = lanczos2(1.f - t);
    w[3] = lanczos2(2.f - t);
    const f32 inv = 1.f / (w[0] + w[1] + w[2] + w[3]);
    for (u32 i = 0; i < 4u; ++i) {
        w[i] *= inv;
    }
}

/// Bilinear sample of an RGB image at continuous pixel coordinates (texel centres at i + 0.5, edge-clamped).
FUSE_HOST_DEVICE inline math::Vec3 sample_bilinear(const math::Vec3* image, u32 width, u32 height, f32 px, f32 py) {
    const f32 fx = px - 0.5f;
    const f32 fy = py - 0.5f;
    const i32 x0 = static_cast<i32>(std::floor(fx));
    const i32 y0 = static_cast<i32>(std::floor(fy));
    const f32 tx = fx - static_cast<f32>(x0);
    const f32 ty = fy - static_cast<f32>(y0);
    const math::Vec3 top = lerp3(texel(image, width, height, x0, y0), texel(image, width, height, x0 + 1, y0), tx);
    const math::Vec3 bottom =
        lerp3(texel(image, width, height, x0, y0 + 1), texel(image, width, height, x0 + 1, y0 + 1), tx);
    return lerp3(top, bottom, ty);
}

/// Separable 4x4 cubic sample (Catmull-Rom or Lanczos-2 weights) of an RGB image, edge-clamped.
FUSE_HOST_DEVICE inline math::Vec3 sample_cubic(const math::Vec3* image, u32 width, u32 height, f32 px, f32 py,
                                                bool lanczos) {
    const f32 fx = px - 0.5f;
    const f32 fy = py - 0.5f;
    const i32 x1 = static_cast<i32>(std::floor(fx));
    const i32 y1 = static_cast<i32>(std::floor(fy));
    f32 wx[4];
    f32 wy[4];
    if (lanczos) {
        lanczos2_weights(fx - static_cast<f32>(x1), wx);
        lanczos2_weights(fy - static_cast<f32>(y1), wy);
    } else {
        catmull_rom_weights(fx - static_cast<f32>(x1), wx);
        catmull_rom_weights(fy - static_cast<f32>(y1), wy);
    }
    math::Vec3 sum{};
    for (i32 j = 0; j < 4; ++j) {
        math::Vec3 row{};
        for (i32 i = 0; i < 4; ++i) {
            row = row + texel(image, width, height, x1 - 1 + i, y1 - 1 + j) * wx[i];
        }
        sum = sum + row * wy[j];
    }
    return sum;
}

/// Catmull-Rom (16-tap) sample (the historical TAA history filter).
FUSE_HOST_DEVICE inline math::Vec3 sample_catmull_rom(const math::Vec3* image, u32 width, u32 height, f32 px, f32 py) {
    return sample_cubic(image, width, height, px, py, false);
}

} // namespace fuse::renderer::taa_common
