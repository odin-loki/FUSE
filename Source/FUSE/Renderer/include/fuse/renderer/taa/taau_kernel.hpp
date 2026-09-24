#pragma once

// Single-source native temporal upsampling (TAAU) — kernel "taau" (docs/compute-kernels.md).
//
// Evolves the same-resolution CPU TAA (taa_cpu_resolve.cpp) to a render -> display resolution resolve. One item
// per DISPLAY pixel:
//   1. Current-frame reconstruction: the 3x3 jittered render samples around the display pixel are weighted by a
//      Lanczos-2 window of their distance to the display-pixel centre, measured in display pixels (jitter-aware:
//      render sample (i, j) lies at (i + 0.5 + jitter) in render space). A small-weight Catmull-Rom spatial
//      estimate of the un-jittered position keeps pixels with no nearby sample defined. Samples are weighted by
//      1 / (1 + luma) (Karis 2014) so bright outliers do not flicker. The summed window weight is the frame's
//      "confidence" for that pixel.
//   2. Reprojection: motion of the closest-depth sample of the 3x3 when it is a silhouette (nearer by
//      `dilate_depth_threshold`; keeps moving edges), else the pixel's own sample; history
//      resampled at display resolution with Catmull-Rom (default), Lanczos-2 or bilinear.
//   3. Rejection: off-screen; depth disocclusion — the current surface, moved into the previous view with the
//      camera transform, must lie in the depth range of the previous 3x3 render footprint (+/- `depth_rejection`);
//      velocity disagreement with the previous frame's motion; reactive mask (drops history weight).
//   4. YCoCg neighbourhood clipping: history clipped towards mean +/- gamma * sigma of the render 3x3 (intersected
//      with its min/max, grown to contain the current reconstruction). Clip strength ramps with motion; static
//      pixels may keep sub-pixel detail (`static_clip_strength`); reactive / transparency masks force clipping.
//   5. Accumulation: history weight + current confidence, capped at `max_accumulation` for static pixels and at
//      max_accumulation / (1 + motion_px * accumulation_motion_falloff) for moving ones (each reprojection
//      resamples the history; long accumulation under motion only integrates that blur). Output = history.
//      No sharpening (a CAS / RCAS pass belongs to the post chain).
// History (display resolution RGB + accumulated weight in w) is stored exposure-normalised (colour * exposure).
//
// Every helper is FUSE_HOST_DEVICE; the host driver is TaauUpscaler (taau.hpp).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/taa/taa_kernel_common.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::taau_kernel {

inline constexpr const char* kName = "taau";
inline constexpr const char* kSpatialName = "upscale_spatial";
inline constexpr kernel::Dim3 kWorkgroup{8u, 8u, 1u};
/// Depth used for sky / no-geometry pixels (depth <= 0) in rejection tests.
inline constexpr f32 kSkyDepth = 1.0e6f;

enum class HistoryFilter : u32 { Bilinear = 0, CatmullRom = 1, Lanczos2 = 2 };
enum class SpatialFilter : u32 { Bilinear = 0, CatmullRom = 1, Lanczos2 = 2 };

/// TAAU tuning (defaults calibrated on the upscaler reference scenes, test_taau_gates.cpp).
struct Settings {
    f32 max_accumulation = 10.f;     ///< History weight cap of static pixels (blend ~ confidence / (cap + confidence)).
    /// Moving pixels cap the history weight at max_accumulation / (1 + motion_px * falloff): every frame of motion
    /// resamples (and slightly blurs) the history, so long accumulation only pays off for (nearly) static pixels.
    f32 accumulation_motion_falloff = 8.f;
    f32 clamp_gamma = 1.25f;         ///< Variance-clip box half extent in standard deviations.
    f32 depth_rejection = 0.05f;     ///< Relative depth mismatch that marks a disocclusion (0 = off).
    f32 velocity_rejection_px = 1.f; ///< Motion disagreement (display px) that fully rejects history (0 = off).
    f32 clip_full_motion_px = 0.25f; ///< Motion (display px) at which clipping reaches full strength.
    f32 static_clip_strength = 0.f;  ///< Clip strength floor for static pixels.
    f32 spatial_weight = 0.05f;      ///< Weight of the spatial estimate in the current reconstruction.
    f32 sample_kernel_scale = 1.f;   ///< Scales display-pixel distances before the Lanczos-2 window.
    f32 reactive_strength = 1.f;     ///< History weight *= 1 - reactive * strength.
    f32 transparency_clip = 1.f;     ///< Clip strength floor from the transparency / composition mask.
    u32 history_filter = static_cast<u32>(HistoryFilter::CatmullRom);
    u32 dilate_motion = 1u;
    /// Dilation takes the closest-depth neighbour's motion only when it is nearer than the pixel's own sample by
    /// this relative margin (a silhouette), not on smooth grazing surfaces whose rows differ in depth and motion.
    f32 dilate_depth_threshold = 0.1f;
};

/// Launch params. Render-resolution spans hold render_w * render_h elements; display ones display_w * display_h.
struct Params {
    kernel::Span<const math::Vec3> color{};        ///< Jittered linear colour (render).
    kernel::Span<const f32> depth{};               ///< Linear view depth, <= 0 = sky (render).
    kernel::Span<const math::Vec2> motion{};       ///< UV motion current - previous (render).
    kernel::Span<const f32> reactive{};            ///< Optional (empty = none).
    kernel::Span<const f32> transparency{};        ///< Optional (empty = none).
    kernel::Span<const f32> prev_depth{};          ///< Previous frame depth (render); empty = no depth test.
    kernel::Span<const math::Vec2> prev_motion{};  ///< Previous frame motion (render); empty = no velocity test.
    kernel::Span<const math::Vec4> history_in{};   ///< Display RGB (exposed) + weight.
    kernel::Span<math::Vec4> history_out{};
    kernel::Span<math::Vec3> output{};             ///< Display linear colour.
    u32 render_w = 0;
    u32 render_h = 0;
    u32 display_w = 0;
    u32 display_h = 0;
    math::Vec2 jitter_px{};
    f32 exposure = 1.f;
    u32 history_valid = 0;
    u32 has_camera = 0;        ///< cur_to_prev_view / tan_half_* valid (camera-aware disocclusion depth).
    f32 tan_half_x = 1.f;
    f32 tan_half_y = 1.f;
    f32 cur_to_prev_view[12] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f}; ///< 3x4 row-major.
    Settings settings{};
};

inline bool params_valid(const Params& p) {
    const u32 rn = p.render_w * p.render_h;
    const u32 dn = p.display_w * p.display_h;
    return p.render_w != 0u && p.render_h != 0u && p.display_w >= p.render_w && p.display_h >= p.render_h &&
           p.color.size >= rn && p.depth.size >= rn && p.motion.size >= rn && p.history_in.size >= dn &&
           p.history_out.size >= dn && p.output.size >= dn && (p.reactive.empty() || p.reactive.size >= rn) &&
           (p.transparency.empty() || p.transparency.size >= rn) && (p.prev_depth.empty() || p.prev_depth.size >= rn) &&
           (p.prev_motion.empty() || p.prev_motion.size >= rn) && p.exposure > 0.f;
}

inline kernel::KernelLaunch make_launch(u32 displayWidth, u32 displayHeight) {
    return kernel::KernelLaunch{kName, kernel::extent2(displayWidth, displayHeight), kWorkgroup};
}

FUSE_HOST_DEVICE inline f32 effective_depth(f32 d) { return d > 0.f ? d : kSkyDepth; }

/// Non-finite / negative colour guard (NaN-safe: comparisons with NaN fail -> 0).
FUSE_HOST_DEVICE inline f32 sanitize1(f32 v) { return (v >= 0.f && v < 3.0e38f) ? v : 0.f; }
FUSE_HOST_DEVICE inline math::Vec3 sanitize(const math::Vec3& c) { return {sanitize1(c.x), sanitize1(c.y), sanitize1(c.z)}; }

/// Jitter-aware sample weight: Lanczos-2 main lobe of the display-pixel distance (0 beyond one display pixel).
FUSE_HOST_DEVICE inline f32 sample_weight(f32 distanceDisplayPx) {
    return distanceDisplayPx < 1.f ? taa_common::lanczos2(distanceDisplayPx) : 0.f;
}

/// History weight at a display position: bilinear over the 2x2 footprint (edge-clamped). (A min over the
/// footprint would compound every frame and pin the accumulated weight near one frame's worth.)
FUSE_HOST_DEVICE inline f32 sample_history_weight(const math::Vec4* h, u32 w, u32 hgt, f32 px, f32 py) {
    const f32 fx = px - 0.5f;
    const f32 fy = py - 0.5f;
    const i32 x0 = static_cast<i32>(std::floor(fx));
    const i32 y0 = static_cast<i32>(std::floor(fy));
    const f32 tx = fx - static_cast<f32>(x0);
    const f32 ty = fy - static_cast<f32>(y0);
    const f32 a = taa_common::texel(h, w, hgt, x0, y0).w;
    const f32 b = taa_common::texel(h, w, hgt, x0 + 1, y0).w;
    const f32 c = taa_common::texel(h, w, hgt, x0, y0 + 1).w;
    const f32 d = taa_common::texel(h, w, hgt, x0 + 1, y0 + 1).w;
    const f32 top = a + (b - a) * tx;
    const f32 bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

/// History colour sample (display pixel coordinates) with the configured filter.
FUSE_HOST_DEVICE inline math::Vec3 sample_history(const math::Vec4* h, u32 w, u32 hgt, f32 px, f32 py, u32 filter) {
    const f32 fx = px - 0.5f;
    const f32 fy = py - 0.5f;
    const i32 x1 = static_cast<i32>(std::floor(fx));
    const i32 y1 = static_cast<i32>(std::floor(fy));
    const f32 tx = fx - static_cast<f32>(x1);
    const f32 ty = fy - static_cast<f32>(y1);
    if (filter == static_cast<u32>(HistoryFilter::Bilinear)) {
        const math::Vec4& a = taa_common::texel(h, w, hgt, x1, y1);
        const math::Vec4& b = taa_common::texel(h, w, hgt, x1 + 1, y1);
        const math::Vec4& c = taa_common::texel(h, w, hgt, x1, y1 + 1);
        const math::Vec4& d = taa_common::texel(h, w, hgt, x1 + 1, y1 + 1);
        const math::Vec3 top = taa_common::lerp3({a.x, a.y, a.z}, {b.x, b.y, b.z}, tx);
        const math::Vec3 bottom = taa_common::lerp3({c.x, c.y, c.z}, {d.x, d.y, d.z}, tx);
        return taa_common::lerp3(top, bottom, ty);
    }
    f32 wx[4];
    f32 wy[4];
    if (filter == static_cast<u32>(HistoryFilter::Lanczos2)) {
        taa_common::lanczos2_weights(tx, wx);
        taa_common::lanczos2_weights(ty, wy);
    } else {
        taa_common::catmull_rom_weights(tx, wx);
        taa_common::catmull_rom_weights(ty, wy);
    }
    math::Vec3 sum{};
    math::Vec3 lo{3.0e38f, 3.0e38f, 3.0e38f};
    math::Vec3 hi{0.f, 0.f, 0.f};
    for (i32 j = 0; j < 4; ++j) {
        math::Vec3 row{};
        for (i32 i = 0; i < 4; ++i) {
            const math::Vec4& t = taa_common::texel(h, w, hgt, x1 - 1 + i, y1 - 1 + j);
            const math::Vec3 c{t.x, t.y, t.z};
            row = row + c * wx[i];
            if (i == 1 || i == 2) {
                if (j == 1 || j == 2) {
                    lo = taa_common::min3(lo, c);
                    hi = taa_common::max3(hi, c);
                }
            }
        }
        sum = sum + row * wy[j];
    }
    // Cubic filters ring: keep the result inside the 2x2 bilinear footprint's range.
    return taa_common::max3(taa_common::min3(sum, hi), lo);
}

/// Spatial (non-temporal) upscale of the jittered render colour at a display pixel.
FUSE_HOST_DEVICE inline math::Vec3 spatial_sample(const math::Vec3* color, u32 rw, u32 rh, f32 qx, f32 qy, u32 filter) {
    if (filter == static_cast<u32>(SpatialFilter::Bilinear)) {
        return taa_common::sample_bilinear(color, rw, rh, qx, qy);
    }
    return taa_common::sample_cubic(color, rw, rh, qx, qy, filter == static_cast<u32>(SpatialFilter::Lanczos2));
}

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        using namespace taa_common;
        const Settings& s = p.settings;
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        const u32 di = y * p.display_w + x;
        const f32 rwf = static_cast<f32>(p.render_w);
        const f32 rhf = static_cast<f32>(p.render_h);
        const f32 dwf = static_cast<f32>(p.display_w);
        const f32 dhf = static_cast<f32>(p.display_h);
        const f32 scaleX = dwf / rwf;
        const f32 scaleY = dhf / rhf;
        const f32 e = p.exposure;

        // Display pixel centre in UV and in (unjittered) render coordinates; q = jittered texel lattice coordinate.
        const f32 u = (static_cast<f32>(x) + 0.5f) / dwf;
        const f32 v = (static_cast<f32>(y) + 0.5f) / dhf;
        const f32 prx = u * rwf;
        const f32 pry = v * rhf;
        const f32 qx = prx - p.jitter_px.x;
        const f32 qy = pry - p.jitter_px.y;
        const i32 maxRx = static_cast<i32>(p.render_w) - 1;
        const i32 maxRy = static_cast<i32>(p.render_h) - 1;
        const i32 cix = clamp_index(static_cast<i32>(std::floor(qx)), maxRx);
        const i32 ciy = clamp_index(static_cast<i32>(std::floor(qy)), maxRy);
        const u32 ci = static_cast<u32>(ciy) * p.render_w + static_cast<u32>(cix);

        // 1. Current-frame samples: jitter-aware weights, neighbourhood statistics, closest-depth motion.
        f32 sumW = 0.f;
        f32 sumWt = 0.f;
        math::Vec3 sumC{};
        math::Vec3 nMin{3.0e38f, 3.0e38f, 3.0e38f};
        math::Vec3 nMax{-3.0e38f, -3.0e38f, -3.0e38f};
        math::Vec3 m1{};
        math::Vec3 m2{};
        const f32 ownDepth = effective_depth(p.depth[ci]);
        f32 closest = ownDepth;
        u32 closestIndex = ci;
        for (i32 dy = -1; dy <= 1; ++dy) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const i32 tx = clamp_index(cix + dx, maxRx);
                const i32 ty = clamp_index(ciy + dy, maxRy);
                const u32 ti = static_cast<u32>(ty) * p.render_w + static_cast<u32>(tx);
                const math::Vec3 c = sanitize(p.color[ti]) * e;
                const f32 ox = (static_cast<f32>(cix + dx) + 0.5f + p.jitter_px.x - prx) * scaleX;
                const f32 oy = (static_cast<f32>(ciy + dy) + 0.5f + p.jitter_px.y - pry) * scaleY;
                const f32 w = sample_weight(std::sqrt(ox * ox + oy * oy) * s.sample_kernel_scale);
                const f32 wt = w / (1.f + luma709(c));
                sumW += w;
                sumWt += wt;
                sumC = sumC + c * wt;
                const math::Vec3 yc = rgb_to_ycocg(c);
                nMin = min3(nMin, yc);
                nMax = max3(nMax, yc);
                m1 = m1 + yc;
                m2 = m2 + mul3(yc, yc);
                if (s.dilate_motion != 0u) {
                    const f32 d = effective_depth(p.depth[ti]);
                    if (d < closest) {
                        closest = d;
                        closestIndex = ti;
                    }
                }
            }
        }
        const math::Vec2 motion =
            closest < ownDepth * (1.f - s.dilate_depth_threshold) ? p.motion[closestIndex] : p.motion[ci];
        // Spatial estimate (un-jittered position), clamped into the neighbourhood range (cubic ringing).
        math::Vec3 spatialY = rgb_to_ycocg(
            sanitize(spatial_sample(p.color.data, p.render_w, p.render_h, qx, qy, static_cast<u32>(SpatialFilter::CatmullRom))) * e);
        spatialY = max3(min3(spatialY, nMax), nMin);
        const math::Vec3 spatial = ycocg_to_rgb(spatialY);
        const f32 spatialWt = s.spatial_weight / (1.f + luma709(spatial));
        const math::Vec3 current = (sumC + spatial * spatialWt) * (1.f / (sumWt + spatialWt));
        const f32 confidence = sumW + s.spatial_weight;

        const f32 reactive = p.reactive.empty() ? 0.f : saturate(p.reactive[ci]);
        const f32 transparency = p.transparency.empty() ? 0.f : saturate(p.transparency[ci]);

        // 2. Reprojection.
        const f32 mvx = motion.x * dwf;
        const f32 mvy = motion.y * dhf;
        const f32 hx = static_cast<f32>(x) + 0.5f - mvx;
        const f32 hy = static_cast<f32>(y) + 0.5f - mvy;
        f32 historyW = 0.f;
        math::Vec3 history{};
        if (p.history_valid != 0u && hx >= 0.f && hy >= 0.f && hx < dwf && hy < dhf) {
            const f32 motionCap = s.max_accumulation / (1.f + std::sqrt(mvx * mvx + mvy * mvy) * s.accumulation_motion_falloff);
            historyW = std::min(sample_history_weight(p.history_in.data, p.display_w, p.display_h, hx, hy), motionCap);
            history = sample_history(p.history_in.data, p.display_w, p.display_h, hx, hy, s.history_filter);

            // 3a. Depth disocclusion: the surface seen by the nearest render sample, moved into the previous view
            //     (camera transform), must lie within the depth range of the previous 3x3 render footprint around
            //     its reprojected position (+/- depth_rejection). A range test tolerates the steep depth gradients
            //     of grazing surfaces and the previous frame's different jitter; a revealed background is farther
            //     than every previous depth there (the occluder), so it still fails.
            const f32 prevRx = (u - motion.x) * rwf;
            const f32 prevRy = (v - motion.y) * rhf;
            if (!p.prev_depth.empty() && s.depth_rejection > 0.f) {
                const f32 d = p.depth[ci];
                f32 expected = effective_depth(d);
                if (p.has_camera != 0u && d > 0.f) {
                    const f32 su = (static_cast<f32>(cix) + 0.5f + p.jitter_px.x) / rwf;
                    const f32 sv = (static_cast<f32>(ciy) + 0.5f + p.jitter_px.y) / rhf;
                    const f32 vx = (2.f * su - 1.f) * p.tan_half_x * d;
                    const f32 vy = (1.f - 2.f * sv) * p.tan_half_y * d;
                    const f32 vz = -d;
                    const f32* m = p.cur_to_prev_view;
                    expected = -(m[8] * vx + m[9] * vy + m[10] * vz + m[11]);
                }
                const i32 pcx = clamp_index(static_cast<i32>(std::floor(prevRx)), maxRx);
                const i32 pcy = clamp_index(static_cast<i32>(std::floor(prevRy)), maxRy);
                f32 lo = 3.0e38f;
                f32 hi = 0.f;
                for (i32 j = -1; j <= 1; ++j) {
                    for (i32 i = -1; i <= 1; ++i) {
                        const u32 pi = static_cast<u32>(clamp_index(pcy + j, maxRy)) * p.render_w +
                                       static_cast<u32>(clamp_index(pcx + i, maxRx));
                        const f32 dp = effective_depth(p.prev_depth[pi]);
                        lo = std::min(lo, dp);
                        hi = std::max(hi, dp);
                    }
                }
                if (expected < lo * (1.f - s.depth_rejection) || expected > hi * (1.f + s.depth_rejection)) {
                    historyW = 0.f;
                }
            }
            // 3b. Velocity disagreement with the motion stored where the history came from.
            if (!p.prev_motion.empty() && s.velocity_rejection_px > 0.f && historyW > 0.f) {
                const u32 pi = static_cast<u32>(clamp_index(static_cast<i32>(std::floor(prevRy)), maxRy)) * p.render_w +
                               static_cast<u32>(clamp_index(static_cast<i32>(std::floor(prevRx)), maxRx));
                const math::Vec2 pm = p.prev_motion[pi];
                const f32 ddx = (motion.x - pm.x) * dwf;
                const f32 ddy = (motion.y - pm.y) * dhf;
                historyW *= 1.f - saturate(std::sqrt(ddx * ddx + ddy * ddy) / s.velocity_rejection_px);
            }
            // 3c. Reactive mask.
            historyW *= 1.f - saturate(reactive * s.reactive_strength);
        }

        // 4. Neighbourhood clipping (YCoCg variance box).
        const math::Vec3 currentY = rgb_to_ycocg(current);
        math::Vec3 resolvedY = currentY;
        if (historyW > 0.f) {
            const math::Vec3 historyY = rgb_to_ycocg(history);
            const math::Vec3 mean = m1 * (1.f / 9.f);
            const math::Vec3 var = m2 * (1.f / 9.f) - mul3(mean, mean);
            const math::Vec3 sigma{std::sqrt(std::max(0.f, var.x)), std::sqrt(std::max(0.f, var.y)),
                                   std::sqrt(std::max(0.f, var.z))};
            math::Vec3 boxMin = max3(nMin, mean - sigma * s.clamp_gamma);
            math::Vec3 boxMax = min3(nMax, mean + sigma * s.clamp_gamma);
            boxMin = min3(min3(boxMin, boxMax), currentY);
            boxMax = max3(max3(boxMin, boxMax), currentY);
            const f32 motionPx = std::sqrt(mvx * mvx + mvy * mvy);
            const f32 clipStrength =
                std::max(std::max(saturate(s.static_clip_strength), saturate(motionPx / std::max(1e-4f, s.clip_full_motion_px))),
                         std::max(reactive > 0.f ? 1.f : 0.f, saturate(transparency * s.transparency_clip)));
            const math::Vec3 clipped = clip_to_aabb(historyY, boxMin, boxMax);
            const math::Vec3 clippedHistory = lerp3(historyY, clipped, clipStrength);
            // 5. Accumulate.
            const f32 alpha = confidence / (historyW + confidence);
            resolvedY = lerp3(clippedHistory, currentY, alpha);
        }
        const math::Vec3 resolved = sanitize(ycocg_to_rgb(resolvedY));
        const f32 newW = std::min(historyW + confidence, s.max_accumulation);
        p.history_out[di] = math::Vec4{resolved, newW};
        p.output[di] = resolved * (1.f / e);
    }
};

/// Spatial-only upscale (baseline for the gates): bilinear / Catmull-Rom / Lanczos-2 of the jittered frame.
struct SpatialParams {
    kernel::Span<const math::Vec3> color{};
    kernel::Span<math::Vec3> output{};
    u32 render_w = 0;
    u32 render_h = 0;
    u32 display_w = 0;
    u32 display_h = 0;
    math::Vec2 jitter_px{};
    u32 filter = static_cast<u32>(SpatialFilter::CatmullRom);
};

struct SpatialKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const SpatialParams& p) const {
        const f32 qx = (static_cast<f32>(idx.global.x) + 0.5f) * static_cast<f32>(p.render_w) / static_cast<f32>(p.display_w) -
                       p.jitter_px.x;
        const f32 qy = (static_cast<f32>(idx.global.y) + 0.5f) * static_cast<f32>(p.render_h) / static_cast<f32>(p.display_h) -
                       p.jitter_px.y;
        p.output[idx.global.y * p.display_w + idx.global.x] =
            sanitize(spatial_sample(p.color.data, p.render_w, p.render_h, qx, qy, p.filter));
    }
};

} // namespace fuse::renderer::taau_kernel
