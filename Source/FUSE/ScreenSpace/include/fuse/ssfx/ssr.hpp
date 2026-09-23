#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/ssfx/ssfx_view.hpp>
#include <fuse/types.hpp>

namespace fuse::ssfx {

/// Screen-space reflection parameters (B5.7 — mirrors `SSRParams` in the plan).
struct SsrParams {
    /// Maximum screen-space march iterations; the pixel stride grows if the ray is longer than this.
    u32 max_steps = 64;
    /// Minimum screen-space stride in pixels.
    f32 stride_px = 1.f;
    /// Depth test tolerance (view-space units) — how far behind a surface the ray may pass and still hit.
    f32 thickness = 0.5f;
    /// Maximum view-space reflection ray length.
    f32 max_distance = 20.f;
    /// Fraction of the screen (per side) over which hits fade out near the border.
    f32 fade_screen_edge = 0.1f;
    /// Bisection iterations refining the hit between the last two march samples.
    u32 refine_steps = 8;
};

/// Result of tracing one reflection ray in screen space.
struct SsrHit {
    bool hit = false;
    /// Continuous pixel coordinates of the hit.
    f32 px = 0.f;
    f32 py = 0.f;
    /// View-space distance from the reflecting point to the hit.
    f32 distance = 0.f;
    /// Scene colour fetched at the hit (black when missed).
    math::Vec3 color{};
    /// Screen-edge and distance fade in [0, 1] — multiply `color` by this for the final contribution.
    f32 confidence = 0.f;
    u32 steps_taken = 0;
};

SsrParams clampSsrParams(const SsrParams& raw);

/// Mirror reflection of `incident` about unit normal `n`.
math::Vec3 ssrReflect(const math::Vec3& incident, const math::Vec3& n);

/// CPU reference of `ssr_kernel` for one pixel: perspective-correct screen-space march (1/Z interpolation)
/// of the view-space mirror ray against the depth buffer, followed by bisection refinement.
/// `sceneColor` is `width * height` linear RGB.
SsrHit ssrTracePixel(const SsfxGBufferView& view, const math::Vec3* sceneColor, const SsrParams& params, u32 x,
                     u32 y);

/// Trace an arbitrary view-space ray (`direction`, need not be unit) from the centre of pixel `(x, y)` with the
/// same screen-space march as `ssrTracePixel` (which calls this with the mirror direction). Used by SSGI.
SsrHit ssrTraceRay(const SsfxGBufferView& view, const math::Vec3* sceneColor, const SsrParams& params, u32 x, u32 y,
                   const math::Vec3& direction);

/// Full-frame CPU SSR. `reflectiveMask` (optional, `width * height`) skips pixels where it is <= 0.
/// Writes reflected colour (unfaded) and confidence per pixel; either output may be null.
bool computeSsrCpu(const SsfxGBufferView& view, const math::Vec3* sceneColor, const SsrParams& params,
                   const f32* reflectiveMask, math::Vec3* colorOut, f32* confidenceOut);

} // namespace fuse::ssfx
