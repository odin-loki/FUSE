#pragma once

// Single-source HBAO (docs/compute-kernels.md): the ONLY implementation of the horizon-based AO integral.
// hbao.cpp (fuse_ssfx full-frame reference + scalar API), fuse_compute's launch_ssao* (CPU backends) and
// Compute/kernels/ssao.cu (CUDA trampoline) all run this code. Device-safe: no allocation, no std::sort,
// fixed-size slice arrays (directions are clamped to 64 -> at most 32 slices).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/ssfx/hbao.hpp>
#include <fuse/ssfx/ssfx_view.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::ssfx::hbao_kernel {

/// Kernel / profiler / GPU-timestamp name (DeferredFramePipeline's ScreenSpaceAo pass).
inline constexpr const char* kName = "screen_space_ao";
/// One pixel per item, 8x8 tiles.
inline constexpr kernel::Dim3 kWorkgroup{8u, 8u, 1u};
/// clampHbaoParams caps `directions` at 64 -> at most 32 two-sided slices.
inline constexpr u32 kMaxSlices = 32u;

inline constexpr f32 kPi = 3.14159265358979323846f;

FUSE_HOST_DEVICE inline f32 saturate(f32 v) {
    return std::max(0.f, std::min(1.f, v));
}

FUSE_HOST_DEVICE inline HbaoParams clamp_params(const HbaoParams& raw) {
    HbaoParams p = raw;
    p.radius = std::max(1e-4f, raw.radius);
    p.bias = std::max(0.f, std::min(0.95f, raw.bias));
    p.directions = std::max(1u, std::min(64u, raw.directions));
    p.steps_per_dir = std::max(1u, std::min(256u, raw.steps_per_dir));
    p.strength = std::max(0.f, raw.strength);
    p.max_radius_px = std::max(1.f, raw.max_radius_px);
    return p;
}

FUSE_HOST_DEVICE inline void tangent_basis(const math::Vec3& n, math::Vec3& t, math::Vec3& b) {
    const math::Vec3 helper = std::fabs(n.x) < 0.9f ? math::Vec3{1.f, 0.f, 0.f} : math::Vec3{0.f, 1.f, 0.f};
    t = math::cross(helper, n).normalized();
    b = math::cross(n, t);
}

/// Normal oriented towards the camera (view-space origin).
FUSE_HOST_DEVICE inline math::Vec3 facing_normal(const SsfxGBufferView& view, u32 x, u32 y, const math::Vec3& p) {
    math::Vec3 n = view.normalAt(x, y).normalized();
    if (n.dot(p) > 0.f) {
        n = n * -1.f;
    }
    return n;
}

/// Cosine-weighted visibility of pixel `(x, y)` in [0, 1] (see hbaoPixelVisibility). `params` must already be
/// clamped (clamp_params).
FUSE_HOST_DEVICE inline f32 pixel_visibility(const SsfxGBufferView& view, const HbaoParams& params, u32 x, u32 y) {
    if (!view.valid() || x >= view.camera.width || y >= view.camera.height || view.depthAt(x, y) <= 0.f) {
        return 1.f;
    }
    const SsfxCamera& cam = view.camera;
    const math::Vec3 p = view.positionAt(x, y);
    const math::Vec3 n = facing_normal(view, x, y, p);
    const math::Vec3 viewVec = (p * -1.f).normalized();

    const f32 radiusPx = std::min(params.max_radius_px, params.radius * cam.fx / p.z);
    if (radiusPx < 1.f) {
        return 1.f;
    }
    const u32 steps = std::max(1u, std::min(params.steps_per_dir, static_cast<u32>(radiusPx)));
    const f32 cxp = static_cast<f32>(x) + 0.5f;
    const f32 cyp = static_cast<f32>(y) + 0.5f;

    // Basis perpendicular to the view vector — measures the angle of each view-aligned slice.
    math::Vec3 viewT{};
    math::Vec3 viewB{};
    tangent_basis(viewVec, viewT, viewB);

    // Each slice is two-sided (screen direction and its opposite), so `directions` horizon searches make
    // `directions / 2` slices.
    const u32 requestedSlices = std::max(1u, params.directions / 2u);
    const u32 sliceCount = requestedSlices < kMaxSlices ? requestedSlices : kMaxSlices;
    f32 sliceAngle[kMaxSlices];
    f32 sliceVisibility[kMaxSlices];

    for (u32 sIdx = 0u; sIdx < sliceCount; ++sIdx) {
        const f32 phi = kPi * (static_cast<f32>(sIdx) + 0.5f) / static_cast<f32>(sliceCount);
        const f32 dx = std::cos(phi);
        const f32 dy = std::sin(phi);

        // Slice plane spanned by the view vector and the screen direction (view +Y is screen-down).
        const math::Vec3 direction{dx, dy, 0.f};
        const math::Vec3 ortho = (direction - viewVec * direction.dot(viewVec)).normalized();
        const math::Vec3 axis = math::cross(ortho, viewVec).normalized();
        const math::Vec3 projN = n - axis * n.dot(axis);
        const f32 projNLen = projN.length();
        sliceAngle[sIdx] = std::atan2(ortho.dot(viewB), ortho.dot(viewT));
        sliceVisibility[sIdx] = 1.f;
        if (projNLen < 1e-6f) {
            sliceVisibility[sIdx] = 0.f;
            continue;
        }
        const f32 signN = ortho.dot(projN) >= 0.f ? 1.f : -1.f;
        const f32 cosN = saturate(projN.dot(viewVec) / projNLen);
        const f32 gamma = signN * std::acos(cosN);

        // Horizon search on both sides; horizons start at the tangent plane (no occlusion).
        f32 horizonCos[2] = {std::cos(gamma + 0.5f * kPi), std::cos(gamma - 0.5f * kPi)};
        for (u32 side = 0u; side < 2u; ++side) {
            const f32 sdx = side == 0u ? dx : -dx;
            const f32 sdy = side == 0u ? dy : -dy;
            i32 lastX = static_cast<i32>(x);
            i32 lastY = static_cast<i32>(y);
            for (u32 s = 1u; s <= steps; ++s) {
                const f32 t = radiusPx * static_cast<f32>(s) / static_cast<f32>(steps);
                const f32 sx = cxp + sdx * t;
                const f32 sy = cyp + sdy * t;
                if (!cam.inside(sx, sy)) {
                    break;
                }
                const i32 ix = static_cast<i32>(sx);
                const i32 iy = static_cast<i32>(sy);
                if (ix == lastX && iy == lastY) {
                    continue;
                }
                lastX = ix;
                lastY = iy;
                const u32 ux = static_cast<u32>(ix);
                const u32 uy = static_cast<u32>(iy);
                if (view.depthAt(ux, uy) <= 0.f) {
                    continue;
                }
                const math::Vec3 h = view.positionAt(ux, uy) - p;
                const f32 dist = h.length();
                if (dist < 1e-6f || dist > params.radius) {
                    continue;
                }
                const math::Vec3 hn = h * (1.f / dist);
                // Angle bias: ignore samples within asin(bias) of the tangent plane (self-occlusion guard).
                if (hn.dot(n) <= params.bias) {
                    continue;
                }
                horizonCos[side] = std::max(horizonCos[side], hn.dot(viewVec));
            }
        }

        // Exact cosine-weighted visibility of the slice between the two horizons (projected-normal form).
        f32 h1 = std::acos(std::max(-1.f, std::min(1.f, horizonCos[0])));
        f32 h0 = -std::acos(std::max(-1.f, std::min(1.f, horizonCos[1])));
        h0 = gamma + std::max(-0.5f * kPi, std::min(0.5f * kPi, h0 - gamma));
        h1 = gamma + std::max(-0.5f * kPi, std::min(0.5f * kPi, h1 - gamma));
        const f32 sinG = std::sin(gamma);
        const f32 arc0 = 0.25f * (cosN + 2.f * h0 * sinG - std::cos(2.f * h0 - gamma));
        const f32 arc1 = 0.25f * (cosN + 2.f * h1 * sinG - std::cos(2.f * h1 - gamma));
        sliceVisibility[sIdx] = projNLen * (arc0 + arc1);
    }

    f32 visibility = 0.f;
    if (params.azimuth_weighting && sliceCount > 1u) {
        // Weight each slice by the angle (around the view vector, period pi) it represents. Slices are
        // ordered by angle with an insertion sort (angles are distinct, so the order is unique).
        for (u32 i = 0u; i < sliceCount; ++i) {
            sliceAngle[i] = std::fmod(sliceAngle[i] + 2.f * kPi, kPi);
        }
        for (u32 i = 1u; i < sliceCount; ++i) {
            const f32 angle = sliceAngle[i];
            const f32 vis = sliceVisibility[i];
            u32 j = i;
            while (j > 0u && angle < sliceAngle[j - 1u]) {
                sliceAngle[j] = sliceAngle[j - 1u];
                sliceVisibility[j] = sliceVisibility[j - 1u];
                --j;
            }
            sliceAngle[j] = angle;
            sliceVisibility[j] = vis;
        }
        f32 totalWeight = 0.f;
        for (u32 i = 0u; i < sliceCount; ++i) {
            const f32 prev = sliceAngle[(i + sliceCount - 1u) % sliceCount];
            const f32 next = sliceAngle[(i + 1u) % sliceCount];
            f32 gap = next - prev;
            if (gap <= 0.f) {
                gap += kPi;
            }
            visibility += 0.5f * gap * sliceVisibility[i];
            totalWeight += 0.5f * gap;
        }
        visibility = totalWeight > 0.f ? visibility / totalWeight : 1.f;
    } else {
        for (u32 i = 0u; i < sliceCount; ++i) {
            visibility += sliceVisibility[i];
        }
        visibility /= static_cast<f32>(sliceCount);
    }

    visibility = saturate(visibility);
    return params.strength == 1.f ? visibility : std::pow(visibility, params.strength);
}

/// Full-frame launch params: view (device-visible depth / normals), clamped HBAO params, visibility out.
struct Params {
    SsfxGBufferView view{};
    HbaoParams hbao{};
    f32* visibility_out = nullptr;
};

inline Params make_params(const SsfxGBufferView& view, const HbaoParams& raw, f32* visibilityOut) {
    return Params{view, clamp_params(raw), visibilityOut};
}

inline kernel::KernelLaunch make_launch(const SsfxGBufferView& view) {
    return kernel::KernelLaunch{kName, kernel::extent2(view.camera.width, view.camera.height), kWorkgroup};
}

/// One pixel: unblurred HBAO visibility (1 for sky).
struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        p.visibility_out[p.view.index(x, y)] = pixel_visibility(p.view, p.hbao, x, y);
    }
};

} // namespace fuse::ssfx::hbao_kernel
