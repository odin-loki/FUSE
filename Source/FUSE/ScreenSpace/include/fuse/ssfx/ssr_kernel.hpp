#pragma once

// Single-source screen-space reflections (docs/compute-kernels.md): the ONLY implementation of the
// perspective-correct screen-space march + bisection (also the ray tracer of SSGI, ssgi_kernel.hpp), the
// roughness mask and the contact-hardened gloss fade. ssr.cpp (fuse_ssfx full-frame reference + scalar API),
// fuse_compute's launch_ssr* (CPU backends) and Compute/kernels/ssr.cu (CUDA trampoline) all run this code.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/ssfx/ssfx_view.hpp>
#include <fuse/ssfx/ssr.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::ssfx::ssr_kernel {

/// Kernel / profiler / GPU-timestamp name of the screen-space reflection pass.
inline constexpr const char* kName = "screen_space_reflections";
/// One pixel per item, 8x8 tiles.
inline constexpr kernel::Dim3 kWorkgroup{8u, 8u, 1u};

FUSE_HOST_DEVICE inline f32 saturate(f32 v) {
    return std::max(0.f, std::min(1.f, v));
}

/// Nearest-pixel depth at continuous coordinates; 0 when off-screen or sky.
FUSE_HOST_DEVICE inline f32 depth_nearest(const SsfxGBufferView& view, f32 px, f32 py) {
    if (!view.camera.inside(px, py)) {
        return 0.f;
    }
    return view.depthAt(static_cast<u32>(px), static_cast<u32>(py));
}

FUSE_HOST_DEVICE inline SsrParams clamp_params(const SsrParams& raw) {
    SsrParams p = raw;
    p.max_steps = std::max(1u, std::min(4096u, raw.max_steps));
    p.stride_px = std::max(0.25f, raw.stride_px);
    p.thickness = std::max(0.f, raw.thickness);
    p.max_distance = std::max(1e-3f, raw.max_distance);
    p.fade_screen_edge = std::max(0.f, std::min(0.5f, raw.fade_screen_edge));
    p.refine_steps = std::min(32u, raw.refine_steps);
    return p;
}

FUSE_HOST_DEVICE inline math::Vec3 reflect(const math::Vec3& incident, const math::Vec3& n) {
    return incident - n * (2.f * incident.dot(n));
}

/// View depth along the projected ray at screen fraction `f` (1/Z is linear in screen space).
FUSE_HOST_DEVICE inline f32 ray_z_at(f32 k0, f32 k1, f32 f) {
    return 1.f / (k0 + (k1 - k0) * f);
}

/// Screen-space march of the view-space ray `direction` (need not be unit) from the centre of pixel `(x, y)`
/// (see ssrTraceRay). `rawParams` is clamped here.
FUSE_HOST_DEVICE inline SsrHit trace_ray(const SsfxGBufferView& view, const math::Vec3* sceneColor,
                                         const SsrParams& rawParams, u32 x, u32 y, const math::Vec3& direction) {
    SsrHit result{};
    if (!view.valid() || sceneColor == nullptr || x >= view.camera.width || y >= view.camera.height ||
        view.depthAt(x, y) <= 0.f || direction.length() <= 0.f) {
        return result;
    }
    const SsrParams params = clamp_params(rawParams);
    const SsfxCamera& cam = view.camera;

    const math::Vec3 p = view.positionAt(x, y);
    const math::Vec3 r = direction.normalized();

    // Clip the ray against the near plane so both endpoints project.
    f32 rayLength = params.max_distance;
    if (r.z < 0.f) {
        const f32 maxLen = (p.z - cam.near_z * 1.01f) / -r.z;
        rayLength = std::min(rayLength, maxLen);
    }
    if (rayLength <= 1e-4f) {
        return result;
    }
    const math::Vec3 e = p + r * rayLength;

    f32 p0x = 0.f;
    f32 p0y = 0.f;
    f32 p1x = 0.f;
    f32 p1y = 0.f;
    if (!cam.project(p, p0x, p0y) || !cam.project(e, p1x, p1y)) {
        return result;
    }
    const f32 k0 = 1.f / p.z;
    const f32 k1 = 1.f / e.z;
    const math::Vec3 q0 = p * k0;
    const math::Vec3 q1 = e * k1;
    const f32 ddx = p1x - p0x;
    const f32 ddy = p1y - p0y;
    const f32 pixelLength = std::max(std::fabs(ddx), std::fabs(ddy));
    if (pixelLength < 1e-3f) {
        return result;
    }
    const f32 stride = std::max(params.stride_px, pixelLength / static_cast<f32>(params.max_steps));
    const u32 stepCount = static_cast<u32>(std::ceil(pixelLength / stride));
    const f32 fStep = stride / pixelLength;

    f32 prevF = 0.f;
    f32 prevZ = p.z;
    bool hit = false;
    f32 hitF = 0.f;
    for (u32 i = 1u; i <= stepCount; ++i) {
        const f32 f = std::min(1.f, static_cast<f32>(i) * fStep);
        const f32 sx = p0x + ddx * f;
        const f32 sy = p0y + ddy * f;
        result.steps_taken = i;
        if (!cam.inside(sx, sy)) {
            break;
        }
        const f32 rayZ = ray_z_at(k0, k1, f);
        const f32 sceneZ = depth_nearest(view, sx, sy);
        if (sceneZ > 0.f && !(static_cast<u32>(sx) == x && static_cast<u32>(sy) == y)) {
            const f32 zMin = std::min(prevZ, rayZ);
            const f32 zMax = std::max(prevZ, rayZ);
            if (zMax >= sceneZ && zMin <= sceneZ + params.thickness) {
                // Bisection between the last two samples for the first crossing.
                f32 lo = prevF;
                f32 hi = f;
                for (u32 k = 0u; k < params.refine_steps; ++k) {
                    const f32 mid = 0.5f * (lo + hi);
                    const f32 midSceneZ = depth_nearest(view, p0x + ddx * mid, p0y + ddy * mid);
                    if (midSceneZ > 0.f && ray_z_at(k0, k1, mid) >= midSceneZ) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                hit = true;
                hitF = hi;
                break;
            }
        }
        prevF = f;
        prevZ = rayZ;
    }
    if (!hit) {
        return result;
    }

    result.hit = true;
    result.px = p0x + ddx * hitF;
    result.py = p0y + ddy * hitF;
    const u32 hx = std::min(cam.width - 1u, static_cast<u32>(std::max(0.f, result.px)));
    const u32 hy = std::min(cam.height - 1u, static_cast<u32>(std::max(0.f, result.py)));
    result.color = sceneColor[view.index(hx, hy)];

    const f32 k = k0 + (k1 - k0) * hitF;
    const math::Vec3 hitPos = (q0 + (q1 - q0) * hitF) * (1.f / k);
    result.distance = (hitPos - p).length();

    f32 edgeFade = 1.f;
    if (params.fade_screen_edge > 0.f) {
        const f32 u = result.px / static_cast<f32>(cam.width);
        const f32 vv = result.py / static_cast<f32>(cam.height);
        const f32 edge = std::min(std::min(u, 1.f - u), std::min(vv, 1.f - vv));
        edgeFade = saturate(edge / params.fade_screen_edge);
    }
    const f32 distanceFade = saturate(4.f * (1.f - result.distance / params.max_distance));
    result.confidence = edgeFade * distanceFade;
    return result;
}

/// Mirror reflection ray of pixel `(x, y)` (see ssrTracePixel).
FUSE_HOST_DEVICE inline SsrHit trace_pixel(const SsfxGBufferView& view, const math::Vec3* sceneColor,
                                           const SsrParams& params, u32 x, u32 y) {
    if (!view.valid() || sceneColor == nullptr || x >= view.camera.width || y >= view.camera.height ||
        view.depthAt(x, y) <= 0.f) {
        return SsrHit{};
    }
    const math::Vec3 p = view.positionAt(x, y);
    math::Vec3 n = view.normalAt(x, y).normalized();
    const math::Vec3 v = p.normalized();
    if (n.dot(v) > 0.f) {
        n = n * -1.f;
    }
    return trace_ray(view, sceneColor, params, x, y, reflect(v, n));
}

/// Contact hardening: roughness pulled towards `floor` as the hit distance shrinks (fuse_compute's
/// ssr_contact_harden_roughness is a wrapper over this).
struct ContactHardening {
    bool enabled = false;
    f32 distance = 0.5f;
    f32 roughness_floor = 0.02f;
    f32 exponent = 2.f;
};

FUSE_HOST_DEVICE inline f32 contact_harden_roughness(f32 rayHitDistance, f32 materialRoughness,
                                                     const ContactHardening& contact) {
    if (!contact.enabled || contact.distance <= 0.f) {
        return std::clamp(materialRoughness, 0.f, 1.f);
    }
    const f32 distance = std::max(rayHitDistance, 0.f);
    const f32 contactFactor = std::pow(std::clamp(1.f - distance / contact.distance, 0.f, 1.f), contact.exponent);
    const f32 hardenedRoughness =
        materialRoughness + (contact.roughness_floor - materialRoughness) * contactFactor;
    return std::clamp(hardenedRoughness, contact.roughness_floor, 1.f);
}

/// Confidence multiplier for a mirror trace standing in for a glossy lobe of `roughness`.
FUSE_HOST_DEVICE inline f32 gloss_fade(const ContactHardening& contact, f32 hitDistance, f32 roughness) {
    const f32 r = std::clamp(roughness, 0.f, 1.f);
    const f32 hardened = r <= contact.roughness_floor ? r : contact_harden_roughness(hitDistance, r, contact);
    return std::clamp(1.f - hardened, 0.f, 1.f);
}

/// Full-frame launch params. Every surface pointer is device-visible on the backend that runs the launch.
struct Params {
    SsfxGBufferView view{};
    const math::Vec3* scene_color = nullptr;
    SsrParams trace{};
    /// Optional: pixels with mask <= 0 are not traced (computeSsrCpu's `reflectiveMask`).
    const f32* reflective_mask = nullptr;
    /// Optional perceptual roughness: pixels with roughness >= 1 are not traced, hits are gloss-faded.
    const f32* roughness = nullptr;
    ContactHardening contact{};
    /// Outputs (each optional): reflected colour (unfaded), confidence, or both packed as rgb + confidence.
    math::Vec3* color_out = nullptr;
    f32* confidence_out = nullptr;
    math::Vec4* rgba_out = nullptr;
};

inline kernel::KernelLaunch make_launch(const SsfxGBufferView& view) {
    return kernel::KernelLaunch{kName, kernel::extent2(view.camera.width, view.camera.height), kWorkgroup};
}

/// One pixel: mask / roughness gate, mirror trace, gloss fade -> rgb = reflected colour (unfaded),
/// w = confidence; misses are zero.
FUSE_HOST_DEVICE inline math::Vec4 shade_pixel(const Params& p, u32 x, u32 y) {
    const u32 i = p.view.index(x, y);
    SsrHit hit{};
    f32 fade = 1.f;
    const f32 r = p.roughness != nullptr ? p.roughness[i] : 0.f;
    if ((p.reflective_mask == nullptr || p.reflective_mask[i] > 0.f) && r < 1.f) {
        hit = trace_pixel(p.view, p.scene_color, p.trace, x, y);
        if (hit.hit && p.roughness != nullptr) {
            fade = gloss_fade(p.contact, hit.distance, r);
        }
    }
    return hit.hit ? math::Vec4{hit.color, hit.confidence * fade} : math::Vec4{};
}

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        const u32 i = p.view.index(x, y);
        const math::Vec4 c = shade_pixel(p, x, y);
        if (p.color_out != nullptr) {
            p.color_out[i] = math::Vec3{c.x, c.y, c.z};
        }
        if (p.confidence_out != nullptr) {
            p.confidence_out[i] = c.w;
        }
        if (p.rgba_out != nullptr) {
            p.rgba_out[i] = c;
        }
    }
};

} // namespace fuse::ssfx::ssr_kernel
