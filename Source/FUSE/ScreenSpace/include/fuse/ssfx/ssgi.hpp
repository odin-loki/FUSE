#pragma once

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/ssfx/ssfx_view.hpp>
#include <fuse/types.hpp>

namespace fuse::ssfx {

/// Screen-space global illumination parameters (B5.7+ quality tier — mirrors `SSGIParams` in Compute).
struct SsgiParams {
    /// Square root of the stratified cosine-weighted rays gathered per pixel (`sample_sqrt^2` rays).
    u32 sample_sqrt = 4;
    /// Diffuse bounces; bounce k re-lights the frame with `direct + indirect_(k-1)`.
    u32 bounces = 1;
    /// Screen-space march per ray (same march as SSR, see `ssrTraceRay`).
    u32 max_steps = 64;
    f32 stride_px = 1.f;
    u32 refine_steps = 6;
    /// Depth tolerance (view-space units) and maximum view-space ray length.
    f32 thickness = 0.5f;
    f32 max_distance = 10.f;
    /// Artistic scale applied to the gathered indirect light (1 = physically based).
    f32 intensity = 1.f;
};

SsgiParams clampSsgiParams(const SsgiParams& raw);

/// Gather direction `(i, j)` of `sampleSqrt^2` for pixel `(x, y)` around the unit normal `n` (any space): cosine-
/// weighted (Malley's method), stratified in both sample dimensions with a deterministic per-pixel, per-stratum
/// jitter (stratum centres would alias against straight edges — a fixed ~2% bias at 64x64 rays).
math::Vec3 ssgiSampleDirection(const math::Vec3& n, u32 x, u32 y, u32 i, u32 j, u32 sampleSqrt);

/// One-bounce screen-space gather for pixel `(x, y)`: the cosine-weighted mean of the radiance seen along the
/// `sample_sqrt^2` `ssgiSampleDirection` rays traced against the depth buffer. A ray contributes
/// the `radiance` of the pixel it hits when the hit surface faces the ray; misses, back faces and off-screen rays
/// contribute zero (no environment term). The result is `E / pi` — multiply by the diffuse albedo to get the
/// outgoing indirect radiance of a Lambertian surface. Sky pixels return zero.
math::Vec3 ssgiPixelGather(const SsfxGBufferView& view, const math::Vec3* radiance, const SsgiParams& params, u32 x,
                           u32 y);

/// Full-frame CPU SSGI. `directRadiance` is the lit scene colour (linear RGB, `width * height`); `albedo` is the
/// diffuse albedo per pixel (null = 1). Writes the outgoing indirect radiance
/// `intensity * albedo * gather(direct + indirect_prev)` after `bounces` iterations. False on bad input.
/// One launch of the single-source `ssgi_kernel` ("screen_space_gi") per bounce on `backend` (CPU backends:
/// host surfaces; CpuReference and CpuParallel are bit-identical).
bool computeSsgiCpu(const SsfxGBufferView& view, const math::Vec3* directRadiance, const math::Vec3* albedo,
                    const SsgiParams& params, math::Vec3* indirectOut,
                    kernel::Backend backend = kernel::Backend::CpuReference);

} // namespace fuse::ssfx
