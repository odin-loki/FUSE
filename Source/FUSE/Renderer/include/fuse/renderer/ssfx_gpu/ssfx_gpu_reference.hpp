#pragma once

// WP-6.3 screen-space fallback on Vulkan: settings, constant resolution and the CPU references (stub-safe).
//
// The oracle of each GPU pass is the single-source CPU kernel it twins, run on the GPU's own prepared inputs
// (read back), so a parity gate isolates the pass:
//   ssfx.prepare  prepare_pixel (below): RT4 device depth -> linear view depth (the WP-2.1 shade's
//                 view_depth_from_device_depth), RT0 signed-octahedral world normal -> view normal in the
//                 ssfx convention, RT1 / RT2 / lit image unpacked
//   ssfx.gtao     fuse::ssfx::gtao_kernel (new, ScreenSpace/include/fuse/ssfx/gtao_kernel.hpp)
//   ssfx.ssr      fuse::ssfx::ssr_kernel (mirror trace, roughness gate + gloss fade, contact hardening)
//   ssfx.ssgi     fuse::ssfx::ssgi_kernel, one pass per bounce (computeSsgiCpu's bounce chain)
//   ssfx.compose  compose_pixel (below)
//
// Composition (integration into the lit image), following the B5 CPU contract of each effect:
//   - AO multiplies the lighting's ambient term, which the WP-2.1 shade evaluates as ambient x albedo x
//     material AO (RT0.w): composed = lit + ambient x albedo x materialAo x (visibility - 1), i.e. the
//     ambient term re-weighted by the GTAO visibility (per channel through gtao_kernel::multi_bounce when
//     enabled); direct light and emissive are untouched.
//   - SSR "multiply colour by confidence for the final contribution" (SsrHit::confidence), weighted by the
//     specular reflectance: Schlick F(F0 = lerp(0.04, albedo, metallic), N.V) with the fifth power as
//     multiplies (as shaders/common/brdf.glsl); the lighting has no specular environment term to replace.
//   - SSGI returns the outgoing indirect radiance intensity x albedo x E / pi (computeSsgiCpu), gathered
//     with the diffuse albedo albedo x (1 - metallic); it is added.
//   - The sum is clamped at 0 (the lit image is half-rounded, so the AO subtraction can dip below 0 by an
//     ulp); alpha is the lit image's (1 = geometry, 0 = sky). Sky pixels are unchanged (AO 1, no hits).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/ssfx_gpu/ssfx_gpu_types.hpp>
#include <fuse/ssfx/gtao_kernel.hpp>
#include <fuse/ssfx/ssfx_view.hpp>
#include <fuse/ssfx/ssgi.hpp>
#include <fuse/ssfx/ssr.hpp>
#include <fuse/ssfx/ssr_kernel.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fuse::renderer::ssfx_gpu {

/// Which passes run and their parameters (the CPU kernels' own parameter structs).
struct SsfxGpuSettings {
    bool ao = true;
    ssfx::GtaoParams gtao{};
    bool multiBounce = false;
    bool ssr = true;
    ssfx::SsrParams ssr_params{};
    /// Gate / gloss-fade SSR by the G-buffer roughness (RT2.x); false = every surface is a mirror.
    bool ssrRoughness = true;
    /// Contact hardening (B5 SSRParams defaults: enabled).
    ssfx::ssr_kernel::ContactHardening contact{true, 0.5f, 0.02f, 2.f};
    bool ssgi = true;
    ssfx::SsgiParams ssgi_params{};
    /// Sky fallback (needs SsfxFrameImages::skyAddress): SSR (kSsfxFlagSky) / SSGI (kSsfxFlagSkyGi) rays that miss and
    /// leave the screen or end over the sky (sky_exit below) return the atmosphere's sky radiance instead of nothing.
    /// Off: the B5 kernels. (A renderer whose other GI already integrates the sky, e.g. DDGI with atmosphere misses,
    /// keeps the SSGI one off: it would add the sky irradiance twice.)
    bool skyFallback = false;
    bool ssgiSkyFallback = false;
};

/// The camera the G-buffer was rendered with. `view` / `proj` are column-major (math::lookAt /
/// math::perspective layout, Vulkan depth range); only the projection intrinsics and the view rotation
/// are used. `nearPlane` / `farPlane` linearise RT4 (forward z/w when !reversedZ, WP-1.3 onwards).
struct SsfxCameraDesc {
    f32 view[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
    f32 proj[16] = {};
    f32 nearPlane = 0.1f;
    f32 farPlane = 1000.f;
    bool reversedZ = false;
};

/// SsfxCamera of a column-major perspective projection (pixel rows grow downwards: view +Y up maps to
/// decreasing rows). False when `proj` is not a perspective projection.
bool camera_from_projection(const f32 (&proj)[16], u32 width, u32 height, ssfx::SsfxCamera& camera);

/// Fills every non-address field of the frame constants (extent, flags, camera, ambient, clamped kernel
/// parameters incl. the GTAO slice table from gtao_kernel::make_params). False on an invalid camera.
bool resolve_constants(const SsfxGpuSettings& settings, const SsfxCameraDesc& camera, const f32 (&ambient)[3],
                       u32 width, u32 height, SsfxFrameConstants& out);

/// The SsfxCamera of resolved constants.
FUSE_HOST_DEVICE inline ssfx::SsfxCamera camera_of(const SsfxFrameConstants& c) {
    ssfx::SsfxCamera cam{};
    cam.width = c.width;
    cam.height = c.height;
    cam.fx = c.fx;
    cam.fy = c.fy;
    cam.cx = c.cx;
    cam.cy = c.cy;
    cam.near_z = c.nearZ;
    return cam;
}

// --- ssfx.prepare reference (per pixel; sx_prepare.{comp,slang} is its twin) ---------------------------

/// Linear view depth of a device depth (the WP-2.1 oracle's clustered_kernel::view_depth_from_device_depth);
/// 0 = sky (cleared value) or invalid.
FUSE_HOST_DEVICE inline f32 linear_depth(f32 deviceDepth, f32 nearPlane, f32 farPlane, bool reversedZ) {
    f32 z = 0.f;
    if (reversedZ) {
        z = deviceDepth > 0.f ? nearPlane / deviceDepth : 0.f;
    } else if (deviceDepth < 1.f && farPlane > nearPlane) {
        z = (nearPlane * farPlane) / (farPlane - deviceDepth * (farPlane - nearPlane));
    }
    return (z > 0.f && z < 3.0e38f) ? z : 0.f;
}

/// Signed-octahedral decode of RT0.xy (shaders/common/gbuffer.glsl oct_decode_signed; normalised as
/// math::Vec3::normalized).
FUSE_HOST_DEVICE inline math::Vec3 oct_decode(f32 ox, f32 oy) {
    math::Vec3 n{ox, oy, 1.f - std::fabs(ox) - std::fabs(oy)};
    if (n.z < 0.f) {
        const f32 x = (1.f - std::fabs(n.y)) * (n.x >= 0.f ? 1.f : -1.f);
        const f32 y = (1.f - std::fabs(n.x)) * (n.y >= 0.f ? 1.f : -1.f);
        n.x = x;
        n.y = y;
    }
    return n.normalized();
}

struct PreparedPixel {
    math::Vec4 prepared{}; ///< linear depth, roughness, metallic, material AO
    math::Vec3 normal{};   ///< ssfx view convention
    math::Vec3 radiance{};
    f32 litAlpha = 0.f;
    math::Vec3 albedo{};
    math::Vec3 diffuse{};
};

/// One pixel of ssfx.prepare from the G-buffer texel values (as the sampled images return them).
FUSE_HOST_DEVICE inline PreparedPixel prepare_pixel(const SsfxFrameConstants& c, f32 rt4, const math::Vec4& rt0,
                                                    const math::Vec4& rt1, const math::Vec4& rt2,
                                                    const math::Vec4& lit) {
    PreparedPixel p{};
    const f32 z = linear_depth(rt4, c.nearPlane, c.farPlane, (c.flags & kSsfxFlagReversedZ) != 0u);
    p.prepared = math::Vec4{z, rt2.x, rt2.y, rt0.w};
    const math::Vec3 w = oct_decode(rt0.x, rt0.y);
    // world -> engine view (+Y up, -Z forward), then the ssfx convention (+Y down, +Z forward).
    const f32 vx = c.viewRot[0] * w.x + c.viewRot[4] * w.y + c.viewRot[8] * w.z;
    const f32 vy = c.viewRot[1] * w.x + c.viewRot[5] * w.y + c.viewRot[9] * w.z;
    const f32 vz = c.viewRot[2] * w.x + c.viewRot[6] * w.y + c.viewRot[10] * w.z;
    p.normal = math::Vec3{vx, -vy, -vz};
    p.radiance = math::Vec3{lit.x, lit.y, lit.z};
    p.litAlpha = lit.w;
    p.albedo = math::Vec3{rt1.x, rt1.y, rt1.z};
    const f32 dw = 1.f - rt2.y;
    p.diffuse = math::Vec3{rt1.x * dw, rt1.y * dw, rt1.z * dw};
    return p;
}

// --- ssfx.compose reference (per pixel; sx_compose.{comp,slang} is its twin) --------------------------

/// Composed radiance (rgb) + lit alpha of pixel (x, y). `prepared` / `normal` / `albedo` / `lit` are the
/// prepare outputs, `ao` / `ssr` / `gi` the effect outputs (ignored when their flag is clear).
FUSE_HOST_DEVICE inline math::Vec4 compose_pixel(const SsfxFrameConstants& c, u32 x, u32 y,
                                                 const math::Vec4& prepared, const math::Vec3& normal,
                                                 const math::Vec3& albedo, const math::Vec3& lit, f32 litAlpha,
                                                 f32 ao, const math::Vec4& ssr, const math::Vec3& gi) {
    f32 r = lit.x;
    f32 g = lit.y;
    f32 b = lit.z;
    if ((c.flags & kSsfxFlagAo) != 0u) {
        f32 vr = ao;
        f32 vg = ao;
        f32 vb = ao;
        if ((c.flags & kSsfxFlagMultiBounce) != 0u) {
            vr = ssfx::gtao_kernel::multi_bounce(ao, albedo.x);
            vg = ssfx::gtao_kernel::multi_bounce(ao, albedo.y);
            vb = ssfx::gtao_kernel::multi_bounce(ao, albedo.z);
        }
        const f32 k = prepared.w;
        r = r + c.ambient[0] * albedo.x * k * (vr - 1.f);
        g = g + c.ambient[1] * albedo.y * k * (vg - 1.f);
        b = b + c.ambient[2] * albedo.z * k * (vb - 1.f);
    }
    if ((c.flags & kSsfxFlagSsr) != 0u && prepared.x > 0.f && ssr.w > 0.f) {
        const math::Vec3 p = camera_of(c).unproject(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f,
                                                    prepared.x);
        math::Vec3 n = normal.normalized();
        if (n.dot(p) > 0.f) {
            n = n * -1.f;
        }
        const math::Vec3 v = (p * -1.f).normalized();
        const f32 nov = std::max(0.f, std::min(1.f, n.dot(v)));
        const f32 m = 1.f - nov;
        const f32 m5 = m * m * m * m * m;
        const f32 metal = prepared.z;
        const f32 f0r = 0.04f * (1.f - metal) + albedo.x * metal;
        const f32 f0g = 0.04f * (1.f - metal) + albedo.y * metal;
        const f32 f0b = 0.04f * (1.f - metal) + albedo.z * metal;
        r = r + ssr.x * ssr.w * (f0r + (1.f - f0r) * m5);
        g = g + ssr.y * ssr.w * (f0g + (1.f - f0g) * m5);
        b = b + ssr.z * ssr.w * (f0b + (1.f - f0b) * m5);
    }
    if ((c.flags & kSsfxFlagSsgi) != 0u) {
        r = r + gi.x;
        g = g + gi.y;
        b = b + gi.z;
    }
    return math::Vec4{std::max(r, 0.f), std::max(g, 0.f), std::max(b, 0.f), litAlpha};
}

// --- sky fallback (kSsfxFlagSky; sx_trace.{glsl,slang} sx_sky_exit / sx_sky_dir twins) ---------------------

/// True when a ray from pixel (x, y) along the view-space `direction` that the march MISSED counts as reaching the
/// sky: the march's own setup (ssr_kernel::trace_ray: near-plane clip, max distance) and its end point leaves the
/// screen or lies over a sky pixel (depth 0). A miss whose end point lies over geometry (the ray ran out of length
/// or passed behind a surface) stays a miss. `maxDistance` = the clamped trace parameter.
FUSE_HOST_DEVICE inline bool sky_exit(const ssfx::SsfxGBufferView& view, f32 maxDistance, u32 x, u32 y,
                                      const math::Vec3& direction) {
    if (!view.valid() || x >= view.camera.width || y >= view.camera.height || view.depthAt(x, y) <= 0.f ||
        direction.length() <= 0.f) {
        return false;
    }
    const ssfx::SsfxCamera& cam = view.camera;
    const math::Vec3 p = view.positionAt(x, y);
    const math::Vec3 r = direction.normalized();
    f32 rayLength = maxDistance;
    if (r.z < 0.f) {
        const f32 maxLen = (p.z - cam.near_z * 1.01f) / -r.z;
        rayLength = std::min(rayLength, maxLen);
    }
    if (rayLength <= 1e-4f) {
        return false;
    }
    const math::Vec3 e = p + r * rayLength;
    f32 ex = 0.f;
    f32 ey = 0.f;
    if (!cam.project(e, ex, ey)) {
        return false;
    }
    if (!cam.inside(ex, ey)) {
        return true;
    }
    return ssfx::ssr_kernel::depth_nearest(view, ex, ey) <= 0.f;
}

/// World direction of a view-space direction in the ssfx convention (+X right, +Y down, +Z forward): the
/// engine view direction (x, -y, -z) through the transpose of the world -> view rotation.
FUSE_HOST_DEVICE inline math::Vec3 sky_world_dir(const SsfxFrameConstants& c, const math::Vec3& d) {
    const f32 ex = d.x;
    const f32 ey = -d.y;
    const f32 ez = -d.z;
    const f32 wx = c.viewRot[0] * ex + c.viewRot[1] * ey + c.viewRot[2] * ez;
    const f32 wy = c.viewRot[4] * ex + c.viewRot[5] * ey + c.viewRot[6] * ez;
    const f32 wz = c.viewRot[8] * ex + c.viewRot[9] * ey + c.viewRot[10] * ez;
    return math::Vec3{wx, wy, wz};
}

/// Mirror reflection direction ssfx.ssr traces from pixel (x, y) (ssr_kernel::trace_pixel's).
FUSE_HOST_DEVICE inline math::Vec3 pixel_reflection(const ssfx::SsfxGBufferView& view, u32 x, u32 y) {
    const math::Vec3 p = view.positionAt(x, y);
    math::Vec3 n = view.normalAt(x, y).normalized();
    const math::Vec3 v = p.normalized();
    if (n.dot(v) > 0.f) {
        n = n * -1.f;
    }
    return ssfx::ssr_kernel::reflect(v, n);
}

/// The CPU side of the sky source: the radiance along a unit world direction (the kernels'
/// at_sky_radiance(atmosphere, dir, false); e.g. atmosphere::at_sky_radiance over the same LUT texels).
struct SsfxSkySource {
    math::Vec3 (*radiance)(const void* user, const math::Vec3& worldDir) = nullptr;
    const void* user = nullptr;
};

// --- full-frame CPU reference --------------------------------------------------------------------------

/// The prepare outputs of a frame (`width * height` each, row 0 = top), e.g. read back from the GPU.
struct SsfxPreparedFrame {
    u32 width = 0;
    u32 height = 0;
    std::vector<math::Vec4> prepared;
    std::vector<math::Vec3> normal;
    std::vector<math::Vec3> radiance;
    std::vector<f32> litAlpha;
    std::vector<math::Vec3> albedo;
    std::vector<math::Vec3> diffuse;
};

struct SsfxReferenceFrame {
    std::vector<f32> ao;               ///< gtao_kernel visibility (1 when AO is off)
    std::vector<math::Vec4> ssr;       ///< ssr_kernel rgb + confidence (0 when SSR is off)
    std::vector<math::Vec3> gi;        ///< computeSsgiCpu (0 when SSGI is off)
    std::vector<math::Vec4> composed;  ///< compose_pixel
};

/// Runs the CPU kernels on `in` with the resolved constants `c` (built from `settings` by
/// resolve_constants): GTAO, SSR, SSGI (all bounces) on `backend`, then compose_pixel. False on bad input.
bool reference_frame(const SsfxGpuSettings& settings, const SsfxFrameConstants& c, const SsfxPreparedFrame& in,
                     SsfxReferenceFrame& out, kernel::Backend backend = kernel::Backend::CpuReference);
/// As above; with a `sky` source, SSR misses that sky_exit return the sky (kSsfxFlagSky; confidence 1 x
/// gloss_fade(maxDistance)) and SSGI misses that sky_exit gather the sky radiance (kSsfxFlagSkyGi; every bounce).
bool reference_frame(const SsfxGpuSettings& settings, const SsfxFrameConstants& c, const SsfxPreparedFrame& in,
                     SsfxReferenceFrame& out, kernel::Backend backend, const SsfxSkySource& sky);

/// View of the prepared depth / normals: `depth` must hold `prepared[i].x` (callers keep it alive).
ssfx::SsfxGBufferView prepared_view(const SsfxFrameConstants& c, const std::vector<f32>& depth,
                                    const std::vector<math::Vec3>& normals);

/// Full-frame CPU GTAO (gtao_kernel on `backend`). False on bad input.
bool computeGtaoCpu(const ssfx::SsfxGBufferView& view, const ssfx::GtaoParams& params, f32* visibilityOut,
                    kernel::Backend backend = kernel::Backend::CpuReference);

} // namespace fuse::renderer::ssfx_gpu
