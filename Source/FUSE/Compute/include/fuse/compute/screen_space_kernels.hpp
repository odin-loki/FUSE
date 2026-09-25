#pragma once

// Single-source SSAO / SSR / SSGI for fuse_compute (docs/compute-kernels.md). The per-pixel bodies are the
// fuse_ssfx kernels — ssfx::hbao_kernel ("screen_space_ao"), ssfx::ssr_kernel ("screen_space_reflections"),
// ssfx::ssgi_kernel ("screen_space_gi", one launch per bounce) — plus the cross-bilateral AO blur defined here
// ("screen_space_ao_blur", a separate 8x8 item launch over the raw AO). This header maps the engine-facing
// SSAOParams / SSRParams / SSGIParams onto those kernels; screen_space_effects_cpu.cpp launches them on the CPU
// backends and kernels/{ssao,ssr,ssgi}.cu stage the surfaces on the device and launch the same bodies.
//
// Consumers must also see fuse_ssfx's include directory (fuse_compute links it privately).

#include <fuse/compute/screen_space_effects.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/ssfx/hbao_kernel.hpp>
#include <fuse/ssfx/ssfx_view.hpp>
#include <fuse/ssfx/ssgi_kernel.hpp>
#include <fuse/ssfx/ssr_kernel.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::compute::screen_space_kernels {

/// Intrinsics of a column-major perspective projection (math::perspective layout: clip.w = -z_view).
/// Pixel rows grow downwards, so view +Y (up) maps to decreasing rows. False when `proj` is not perspective.
inline bool camera_from_projection(const f32 (&proj)[16], u32 width, u32 height, ssfx::SsfxCamera& camera) {
    if (width == 0u || height == 0u || !(proj[0] > 0.f) || !(proj[5] > 0.f) || std::fabs(proj[11] + 1.f) > 1e-4f) {
        return false;
    }
    camera.width = width;
    camera.height = height;
    camera.fx = 0.5f * static_cast<f32>(width) * proj[0];
    camera.fy = 0.5f * static_cast<f32>(height) * proj[5];
    camera.cx = 0.5f * static_cast<f32>(width) * (1.f - proj[8]);
    camera.cy = 0.5f * static_cast<f32>(height) * (1.f + proj[9]);
    // Vulkan depth range: z_ndc = 0 at the near plane, so near = proj[14] / proj[10].
    const f32 nearZ = proj[10] != 0.f ? proj[14] / proj[10] : 0.f;
    camera.near_z = nearZ > 0.f ? nearZ : 0.01f;
    return camera.valid();
}

/// G-buffer view over the depth / normal surfaces (host or device pointers). Engine view-space normals are
/// converted to the ssfx convention (+Y down, +Z forward) on read (`engine_normals`), so nothing is copied.
inline bool make_view(const f32 (&proj)[16], u32 width, u32 height, const void* depthSurface,
                      const void* normalSurface, ssfx::SsfxGBufferView& view) {
    if (depthSurface == nullptr || !camera_from_projection(proj, width, height, view.camera)) {
        return false;
    }
    view.depth = static_cast<const f32*>(depthSurface);
    view.normals = static_cast<const math::Vec3*>(normalSurface);
    view.engine_normals = true;
    return view.valid();
}

inline ssfx::HbaoParams to_hbao(const SSAOParams& params) {
    ssfx::HbaoParams hbao{};
    hbao.radius = params.radius;
    hbao.bias = params.bias;
    hbao.directions = params.directions;
    hbao.steps_per_dir = params.steps_per_dir;
    hbao.strength = params.strength;
    hbao.max_radius_px = params.max_radius_px;
    return hbao;
}

inline ssfx::SsrParams to_ssr(const SSRParams& params) {
    ssfx::SsrParams ssr{};
    ssr.max_steps = params.max_steps;
    ssr.stride_px = params.ray_step_size;
    ssr.thickness = params.thickness;
    ssr.max_distance = params.max_distance;
    ssr.fade_screen_edge = params.fade_screen_edge;
    ssr.refine_steps = params.refine_steps;
    return ssr;
}

inline ssfx::ssr_kernel::ContactHardening to_contact(const SSRParams& params) {
    ssfx::ssr_kernel::ContactHardening contact{};
    contact.enabled = params.contact_hardening;
    contact.distance = params.contact_distance;
    contact.roughness_floor = params.contact_roughness_floor;
    contact.exponent = params.contact_harden_exponent;
    return contact;
}

inline ssfx::SsgiParams to_ssgi(const SSGIParams& params) {
    ssfx::SsgiParams ssgi{};
    ssgi.sample_sqrt = params.sample_sqrt;
    ssgi.bounces = params.max_bounces;
    ssgi.max_steps = params.max_steps;
    ssgi.stride_px = params.ray_step_size;
    ssgi.thickness = params.thickness;
    ssgi.max_distance = params.max_distance;
    ssgi.intensity = params.intensity;
    return ssgi;
}

/// SSR kernel params writing rgb + confidence (SSRParams::ssr_out_surface layout). Surface pointers are the
/// caller's (host for the CPU backends, device for CUDA); `view` must point at the same memory space.
inline ssfx::ssr_kernel::Params make_ssr_params(const ssfx::SsfxGBufferView& view, const SSRParams& params,
                                                const math::Vec3* sceneColor, const f32* roughness,
                                                math::Vec4* out) {
    ssfx::ssr_kernel::Params kp{};
    kp.view = view;
    kp.scene_color = sceneColor;
    kp.trace = to_ssr(params);
    kp.roughness = roughness;
    kp.contact = to_contact(params);
    kp.rgba_out = out;
    return kp;
}

// ---------------------------------------------------------------------------------------------------------
// Cross-bilateral AO blur (P5 §5.7 `hbao_blur_kernel`).

namespace ao_blur {

/// Kernel / profiler name: the blur half of the ScreenSpaceAo pass.
inline constexpr const char* kName = "screen_space_ao_blur";
inline constexpr kernel::Dim3 kWorkgroup{8u, 8u, 1u};
/// 5x5 taps.
inline constexpr i32 kRadius = 2;

struct Thresholds {
    bool enabled = true;
    f32 depth = 0.001f;
    f32 normal = 0.95f;
};

inline Thresholds thresholds(const SSAOParams& params) {
    return Thresholds{params.enable_blur, params.blur_depth_threshold, params.blur_normal_threshold};
}

/// Cross-bilateral tap weight (compute::ssao_blur_weight is a wrapper over this).
FUSE_HOST_DEVICE inline f32 weight(f32 centerDepth, f32 neighborDepth, f32 centerNormalZ, f32 neighborNormalZ,
                                   const Thresholds& t) {
    if (!t.enabled) {
        return 0.f;
    }
    const f32 depthDelta = std::fabs(centerDepth - neighborDepth);
    if (depthDelta > t.depth) {
        return 0.f;
    }
    const f32 normalSimilarity = centerNormalZ * neighborNormalZ;
    if (normalSimilarity < t.normal) {
        return 0.f;
    }
    const f32 depthWeight = 1.f - depthDelta / std::max(t.depth, 1e-6f);
    const f32 normalWeight = (normalSimilarity - t.normal) / std::max(1.f - t.normal, 1e-6f);
    return std::clamp(depthWeight * normalWeight, 0.f, 1.f);
}

struct Params {
    ssfx::SsfxGBufferView view{};
    Thresholds thresholds{};
    const f32* raw = nullptr; ///< Unblurred visibility (hbao_kernel output); must not alias `out`.
    f32* out = nullptr;
};

inline kernel::KernelLaunch make_launch(const ssfx::SsfxGBufferView& view) {
    return kernel::KernelLaunch{kName, kernel::extent2(view.camera.width, view.camera.height), kWorkgroup};
}

/// One pixel: 5x5 taps weighted by the neighbour's distance from the centre tangent plane (relative to the
/// centre depth) and the normal dot product; sky pixels pass through.
struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const ssfx::SsfxGBufferView& view = p.view;
        const i32 x = static_cast<i32>(idx.global.x);
        const i32 y = static_cast<i32>(idx.global.y);
        const i32 w = static_cast<i32>(view.camera.width);
        const i32 h = static_cast<i32>(view.camera.height);
        const u32 ci = view.index(static_cast<u32>(x), static_cast<u32>(y));
        const f32 cz = view.depth[ci];
        if (cz <= 0.f) {
            p.out[ci] = p.raw[ci];
            return;
        }
        const math::Vec3 cp = view.positionAt(static_cast<u32>(x), static_cast<u32>(y));
        const math::Vec3 cn = view.normalAt(static_cast<u32>(x), static_cast<u32>(y)).normalized();
        f32 sum = p.raw[ci];
        f32 total = 1.f;
        for (i32 dy = -kRadius; dy <= kRadius; ++dy) {
            for (i32 dx = -kRadius; dx <= kRadius; ++dx) {
                const i32 nx = x + dx;
                const i32 ny = y + dy;
                if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h) {
                    continue;
                }
                const u32 ni = view.index(static_cast<u32>(nx), static_cast<u32>(ny));
                if (view.depth[ni] <= 0.f) {
                    continue;
                }
                const math::Vec3 np = view.positionAt(static_cast<u32>(nx), static_cast<u32>(ny));
                const math::Vec3 nn = view.normalAt(static_cast<u32>(nx), static_cast<u32>(ny)).normalized();
                const f32 planeDelta = std::fabs(cn.dot(np - cp)) / cz;
                const f32 wgt = weight(0.f, planeDelta, 1.f, cn.dot(nn), p.thresholds);
                sum += wgt * p.raw[ni];
                total += wgt;
            }
        }
        p.out[ci] = sum / total;
    }
};

} // namespace ao_blur

} // namespace fuse::compute::screen_space_kernels
