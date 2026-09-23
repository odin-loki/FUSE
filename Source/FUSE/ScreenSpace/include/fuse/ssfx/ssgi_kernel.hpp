#pragma once

// Single-source screen-space GI (docs/compute-kernels.md): the ONLY implementation of the stratified
// cosine-weighted gather (rays traced with ssr_kernel::trace_ray) and of the per-bounce re-lighting. ssgi.cpp
// (fuse_ssfx full-frame reference + scalar API), fuse_compute's launch_ssgi* (CPU backends) and
// Compute/kernels/ssgi.cu (CUDA trampoline) all run this code. Noise is an integer hash (bit-exact everywhere).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/ssfx/ssfx_view.hpp>
#include <fuse/ssfx/ssgi.hpp>
#include <fuse/ssfx/ssr_kernel.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::ssfx::ssgi_kernel {

/// Kernel / profiler / GPU-timestamp name of the screen-space GI gather (one launch per bounce).
inline constexpr const char* kName = "screen_space_gi";
/// One pixel per item, 8x8 tiles.
inline constexpr kernel::Dim3 kWorkgroup{8u, 8u, 1u};

inline constexpr f32 kTwoPi = 6.28318530717958647692f;

/// Integer hash (lowbias32) -> uniform [0, 1).
FUSE_HOST_DEVICE inline f32 hash_unit(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return static_cast<f32>(v >> 8) * (1.f / 16777216.f);
}

/// See ssgiSampleDirection.
FUSE_HOST_DEVICE inline math::Vec3 sample_direction(const math::Vec3& n, u32 x, u32 y, u32 i, u32 j, u32 sampleSqrt) {
    const u32 k = std::max(1u, sampleSqrt);
    const u32 seed = (x * 73856093U) ^ (y * 19349663U) ^ ((i * k + j) * 83492791U);
    const f32 u1 = (static_cast<f32>(i) + hash_unit(seed)) / static_cast<f32>(k);
    const f32 u2 = (static_cast<f32>(j) + hash_unit(seed ^ 0x9e3779b9U)) / static_cast<f32>(k);
    const math::Vec3 helper = std::fabs(n.x) < 0.9f ? math::Vec3{1.f, 0.f, 0.f} : math::Vec3{0.f, 1.f, 0.f};
    const math::Vec3 tangent = math::cross(helper, n).normalized();
    const math::Vec3 bitangent = math::cross(n, tangent);
    const f32 r = std::sqrt(u1);
    const f32 phi = kTwoPi * u2;
    const f32 lz = std::sqrt(std::max(0.f, 1.f - u1));
    return tangent * (r * std::cos(phi)) + bitangent * (r * std::sin(phi)) + n * lz;
}

FUSE_HOST_DEVICE inline SsgiParams clamp_params(const SsgiParams& raw) {
    SsgiParams p = raw;
    p.sample_sqrt = std::max(1u, std::min(64u, raw.sample_sqrt));
    p.bounces = std::min(8u, raw.bounces);
    p.max_steps = std::max(1u, std::min(4096u, raw.max_steps));
    p.stride_px = std::max(0.25f, raw.stride_px);
    p.refine_steps = std::min(32u, raw.refine_steps);
    p.thickness = std::max(0.f, raw.thickness);
    p.max_distance = std::max(1e-3f, raw.max_distance);
    p.intensity = std::max(0.f, raw.intensity);
    return p;
}

/// The SSR march configured for diffuse gather rays (no screen-edge fade).
FUSE_HOST_DEVICE inline SsrParams trace_params(const SsgiParams& params) {
    SsrParams trace{};
    trace.max_steps = params.max_steps;
    trace.stride_px = params.stride_px;
    trace.thickness = params.thickness;
    trace.max_distance = params.max_distance;
    trace.fade_screen_edge = 0.f;
    trace.refine_steps = params.refine_steps;
    return trace;
}

/// One-bounce gather of pixel `(x, y)` = E / pi (see ssgiPixelGather). `params` must already be clamped.
FUSE_HOST_DEVICE inline math::Vec3 pixel_gather(const SsfxGBufferView& view, const math::Vec3* radiance,
                                                const SsgiParams& params, u32 x, u32 y) {
    if (!view.valid() || radiance == nullptr || x >= view.camera.width || y >= view.camera.height ||
        view.depthAt(x, y) <= 0.f) {
        return {};
    }
    const SsrParams trace = trace_params(params);
    const math::Vec3 p = view.positionAt(x, y);
    math::Vec3 n = view.normalAt(x, y).normalized();
    if (n.dot(p) > 0.f) {
        n = n * -1.f;
    }

    math::Vec3 sum{};
    const u32 total = params.sample_sqrt * params.sample_sqrt;
    for (u32 i = 0u; i < params.sample_sqrt; ++i) {
        for (u32 j = 0u; j < params.sample_sqrt; ++j) {
            // Cosine-weighted directions: the estimator of E / pi is the plain mean of the hit radiance.
            const math::Vec3 dir = sample_direction(n, x, y, i, j, params.sample_sqrt);
            const SsrHit hit = ssr_kernel::trace_ray(view, radiance, trace, x, y, dir);
            if (!hit.hit) {
                continue;
            }
            const u32 hx = std::min(view.camera.width - 1u, static_cast<u32>(std::max(0.f, hit.px)));
            const u32 hy = std::min(view.camera.height - 1u, static_cast<u32>(std::max(0.f, hit.py)));
            // Only the front of the hit surface emits towards the ray origin.
            math::Vec3 hitN = view.normalAt(hx, hy).normalized();
            if (hitN.dot(view.positionAt(hx, hy)) > 0.f) {
                hitN = hitN * -1.f;
            }
            if (hitN.dot(dir) >= 0.f) {
                continue;
            }
            sum = sum + hit.color;
        }
    }
    return sum * (1.f / static_cast<f32>(total));
}

/// One bounce: gathers `radiance_in` and writes the outgoing indirect radiance `intensity * albedo * gather`,
/// plus the next bounce's light source `direct + indirect` when `radiance_out` is set. Every pointer is
/// device-visible on the backend that runs the launch; `radiance_in` must not alias an output.
struct Params {
    SsfxGBufferView view{};
    SsgiParams ssgi{}; ///< Clamped (clamp_params).
    const math::Vec3* radiance_in = nullptr;
    const math::Vec3* direct = nullptr;
    const math::Vec3* albedo = nullptr; ///< Null = 1.
    math::Vec3* indirect_out = nullptr;
    math::Vec3* radiance_out = nullptr; ///< Null on the last bounce.
};

inline kernel::KernelLaunch make_launch(const SsfxGBufferView& view) {
    return kernel::KernelLaunch{kName, kernel::extent2(view.camera.width, view.camera.height), kWorkgroup};
}

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        const u32 i = p.view.index(x, y);
        const math::Vec3 gathered = pixel_gather(p.view, p.radiance_in, p.ssgi, x, y);
        const math::Vec3 a = p.albedo != nullptr ? p.albedo[i] : math::Vec3{1.f, 1.f, 1.f};
        const math::Vec3 indirect = math::Vec3{a.x * gathered.x, a.y * gathered.y, a.z * gathered.z} * p.ssgi.intensity;
        p.indirect_out[i] = indirect;
        if (p.radiance_out != nullptr) {
            p.radiance_out[i] = p.direct[i] + indirect;
        }
    }
};

/// Radiance ping-pong buffers the bounce chain needs besides the inputs and the output (0, 1 or 2).
inline u32 scratch_buffers(u32 bounces) {
    return bounces <= 1u ? 0u : (bounces == 2u ? 1u : 2u);
}

/// Host driver shared by the CPU and CUDA paths: `base.ssgi.bounces` launches of Kernel. Bounce 0 gathers
/// `base.direct`; bounce b > 0 gathers `direct + indirect_(b-1)` from the ping-pong `scratch[]` buffers
/// (scratch_buffers(bounces) of them, `width * height` each). `launch_bounce(const Params&)` runs one launch
/// on the caller's backend and returns false on failure. The caller handles bounces == 0 (zero output).
template <typename LaunchBounce>
bool run_bounces(const Params& base, math::Vec3* scratch0, math::Vec3* scratch1, LaunchBounce&& launch_bounce) {
    Params p = base;
    p.radiance_in = base.direct;
    math::Vec3* next = scratch0;
    math::Vec3* spare = scratch1;
    for (u32 bounce = 0u; bounce < base.ssgi.bounces; ++bounce) {
        const bool last = bounce + 1u == base.ssgi.bounces;
        p.radiance_out = last ? nullptr : next;
        if (!launch_bounce(p)) {
            return false;
        }
        if (!last) {
            p.radiance_in = next;
            math::Vec3* const written = next;
            next = spare;
            spare = written;
        }
    }
    return true;
}

} // namespace fuse::ssfx::ssgi_kernel
