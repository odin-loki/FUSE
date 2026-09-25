#pragma once

// Single-source GTAO (ground-truth ambient occlusion, Jimenez et al. 2016; docs/compute-kernels.md): the ONLY
// implementation of the GTAO slice integral. The renderer's CPU reference (renderer/ssfx_gpu) launches this
// body through kernel::launch; the Vulkan compute kernel shaders/ssfx/sx_gtao.{comp,slang} is its line-for-line
// twin (WP-6.3). HBAO (hbao_kernel.hpp) stays unchanged as the independent oracle.
//
// Differences from hbao_kernel (same height-field assumption, same horizon search rules):
//   - Slices are spread uniformly in angle AROUND THE VIEW VECTOR (not in screen space): slice k is the plane
//     through the view vector and the unit direction d_k = cos(phi_k) T + sin(phi_k) B (T, B perpendicular to
//     the view vector); its screen-space march direction is the projection of d_k at the pixel. Uniform slice
//     angles are the exact measure of the GTAO integral, so no azimuth re-weighting is needed.
//   - Ratio estimator: visibility = sum_k |projN_k| (a(h0_k) + a(h1_k)) / sum_k |projN_k| (a(g_k - pi/2) +
//     a(g_k + pi/2)). The denominator is the same slice quadrature of the unoccluded hemisphere (it tends to 1
//     as the slice count grows), so a flat surface gives exactly 1 for any slice count and tilt, and the
//     estimate converges to the cosine-weighted visibility.
//   - Optional distance falloff: a sample within `falloff * radius` of the radius has its horizon lerped
//     towards the tangent plane (falloff 0 = HBAO's hard radius cut).
//   - Optional per-pixel jitter (integer hash: slice rotation + march offset), deterministic everywhere.
//   - Transcendentals: the slice table (cos / sin of phi_k) is built once on the host (make_params); acos and
//     cos in the integral are minimax / Taylor polynomials built from + - * / sqrt only (acos: Abramowitz &
//     Stegun 4.4.46, |error| <= 2e-8; cos: degree-12 Taylor on [0, pi/2], |error| < 1e-8). IEEE-exact
//     operations only, so the CPU backends and the Vulkan kernel agree bit for bit when neither side contracts
//     multiply-adds (Vulkan's own acos / cos precision is only loosely specified).
//
// Analytic references (gated in tests/test_rp_ssfx_gpu_cpu.cpp and on Lavapipe): a plane gives 1; a point on
// one face of a dihedral wedge of opening angle alpha (infinite faces, radius and screen large enough) gives
// (1 - cos alpha) / 2 (cosine-weighted fraction of the hemisphere not behind the other face).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/ssfx/ssfx_view.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::ssfx {

/// GTAO parameters.
struct GtaoParams {
    /// View-space sample radius; occluders further than this from the shaded point are ignored.
    f32 radius = 1.f;
    /// Fraction of `radius` over which a sample's horizon fades to the tangent plane (0 = hard cut).
    f32 falloff = 0.f;
    /// Samples within asin(bias) of the tangent plane are ignored (self-occlusion guard, as HBAO). Needed with
    /// real G-buffer normals: the RGBA16F octahedral normal is off by up to ~1e-3 rad, and a coplanar sample
    /// snapped to a pixel centre off the slice plane would otherwise raise the horizon at first order.
    f32 bias = 0.02f;
    /// Two-sided slices per pixel (clamped to [1, 32]).
    u32 slices = 4;
    /// Horizon samples per side of a slice (clamped to [1, 256] and to the projected radius in pixels).
    u32 steps = 8;
    /// Visibility exponent (1 = physically based).
    f32 strength = 1.f;
    /// Screen-space clamp on the projected radius.
    f32 max_radius_px = 64.f;
    /// Per-pixel slice rotation + march offset from an integer hash of (x, y, frame).
    bool jitter = false;
    u32 frame = 0;
};

namespace gtao_kernel {

/// Kernel / profiler / GPU-timestamp name (the Vulkan pass is "ssfx.gtao").
inline constexpr const char* kName = "screen_space_gtao";
/// One pixel per item, 8x8 tiles.
inline constexpr kernel::Dim3 kWorkgroup{8u, 8u, 1u};
inline constexpr u32 kMaxSlices = 32u;

inline constexpr f32 kPi = 3.14159265358979323846f;
inline constexpr f32 kHalfPi = 1.57079632679489661923f;
inline constexpr f32 kTwoPi = 6.28318530717958647692f;

FUSE_HOST_DEVICE inline f32 saturate(f32 v) {
    return std::max(0.f, std::min(1.f, v));
}

/// acos on [-1, 1] (A&S 4.4.46, |error| <= 2e-8 on [0, 1]; acos(-x) = pi - acos(x)). Input is clamped.
FUSE_HOST_DEVICE inline f32 acos_poly(f32 x) {
    const f32 c = std::max(-1.f, std::min(1.f, x));
    const f32 a = std::fabs(c);
    f32 p = -0.0012624911f;
    p = p * a + 0.0066700901f;
    p = p * a - 0.0170881256f;
    p = p * a + 0.0308918810f;
    p = p * a - 0.0501743046f;
    p = p * a + 0.0889789874f;
    p = p * a - 0.2145988016f;
    p = p * a + 1.5707963050f;
    const f32 r = std::sqrt(1.f - a) * p;
    return c < 0.f ? kPi - r : r;
}

/// cos on [-2 pi, 2 pi] (folded to [0, pi / 2], degree-12 Taylor polynomial).
FUSE_HOST_DEVICE inline f32 cos_poly(f32 x) {
    f32 a = std::fabs(x);
    if (a > kPi) {
        a = kTwoPi - a;
    }
    f32 sign = 1.f;
    if (a > kHalfPi) {
        a = kPi - a;
        sign = -1.f;
    }
    const f32 a2 = a * a;
    // 1 / 12!, 1 / 10!, 1 / 8!, 1 / 6!, 1 / 4! as exact f32 literals (the shaders use the same digits).
    f32 p = 2.08767559e-09f;
    p = p * a2 - 2.755732e-07f;
    p = p * a2 + 2.48015876e-05f;
    p = p * a2 - 0.00138888892f;
    p = p * a2 + 0.0416666679f;
    p = p * a2 - 0.5f;
    p = p * a2 + 1.f;
    return sign * p;
}

/// Integer hash (lowbias32) -> uniform [0, 1) (the ssgi_kernel hash).
FUSE_HOST_DEVICE inline f32 hash_unit(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return static_cast<f32>(v >> 8) * (1.f / 16777216.f);
}

FUSE_HOST_DEVICE inline GtaoParams clamp_params(const GtaoParams& raw) {
    GtaoParams p = raw;
    p.radius = std::max(1e-4f, raw.radius);
    p.falloff = std::max(0.f, std::min(1.f, raw.falloff));
    p.bias = std::max(0.f, std::min(0.95f, raw.bias));
    p.slices = std::max(1u, std::min(kMaxSlices, raw.slices));
    p.steps = std::max(1u, std::min(256u, raw.steps));
    p.strength = std::max(0.f, raw.strength);
    p.max_radius_px = std::max(1.f, raw.max_radius_px);
    return p;
}

FUSE_HOST_DEVICE inline void tangent_basis(const math::Vec3& n, math::Vec3& t, math::Vec3& b) {
    const math::Vec3 helper = std::fabs(n.x) < 0.9f ? math::Vec3{1.f, 0.f, 0.f} : math::Vec3{0.f, 1.f, 0.f};
    t = math::cross(helper, n).normalized();
    b = math::cross(n, t);
}

/// Cosine-weighted arc integral of one side of a slice up to horizon angle `h` (GTAO paper eq. 7 form).
FUSE_HOST_DEVICE inline f32 arc(f32 h, f32 gamma, f32 cosN, f32 sinG) {
    return 0.25f * (cosN + 2.f * h * sinG - cos_poly(2.f * h - gamma));
}

/// Jimenez et al. 2016 multi-bounce fit: visibility -> visibility with interreflection for albedo `a`.
FUSE_HOST_DEVICE inline f32 multi_bounce(f32 visibility, f32 albedo) {
    const f32 a = 2.0404f * albedo - 0.3324f;
    const f32 b = -4.7951f * albedo + 0.6417f;
    const f32 c = 2.7552f * albedo + 0.6903f;
    return std::max(visibility, ((visibility * a + b) * visibility + c) * visibility);
}

/// Full-frame launch params: view (device-visible depth / normals), clamped GTAO params, the host-built slice
/// table and the visibility output. Build with make_params.
struct Params {
    SsfxGBufferView view{};
    GtaoParams gtao{};
    /// cos / sin of phi_k = pi (k + 0.5) / slices (host libm, once per launch).
    f32 slice_cos[kMaxSlices] = {};
    f32 slice_sin[kMaxSlices] = {};
    f32* visibility_out = nullptr;
};

inline Params make_params(const SsfxGBufferView& view, const GtaoParams& raw, f32* visibilityOut) {
    Params p{};
    p.view = view;
    p.gtao = clamp_params(raw);
    for (u32 k = 0u; k < p.gtao.slices; ++k) {
        const f32 phi = kPi * (static_cast<f32>(k) + 0.5f) / static_cast<f32>(p.gtao.slices);
        p.slice_cos[k] = std::cos(phi);
        p.slice_sin[k] = std::sin(phi);
    }
    p.visibility_out = visibilityOut;
    return p;
}

inline kernel::KernelLaunch make_launch(const SsfxGBufferView& view) {
    return kernel::KernelLaunch{kName, kernel::extent2(view.camera.width, view.camera.height), kWorkgroup};
}

/// GTAO visibility of pixel `(x, y)` in [0, 1] (1 = unoccluded, sky = 1). `p.gtao` is clamped.
FUSE_HOST_DEVICE inline f32 pixel_visibility(const Params& prm, u32 x, u32 y) {
    const SsfxGBufferView& view = prm.view;
    const GtaoParams& params = prm.gtao;
    if (!view.valid() || x >= view.camera.width || y >= view.camera.height || view.depthAt(x, y) <= 0.f) {
        return 1.f;
    }
    const SsfxCamera& cam = view.camera;
    const math::Vec3 p = view.positionAt(x, y);
    math::Vec3 n = view.normalAt(x, y).normalized();
    if (n.dot(p) > 0.f) {
        n = n * -1.f;
    }
    const math::Vec3 viewVec = (p * -1.f).normalized();

    const f32 radiusPx = std::min(params.max_radius_px, params.radius * cam.fx / p.z);
    if (radiusPx < 1.f) {
        return 1.f;
    }
    const u32 steps = std::max(1u, std::min(params.steps, static_cast<u32>(radiusPx)));
    const f32 cxp = static_cast<f32>(x) + 0.5f;
    const f32 cyp = static_cast<f32>(y) + 0.5f;

    math::Vec3 viewT{};
    math::Vec3 viewB{};
    tangent_basis(viewVec, viewT, viewB);

    // Jitter: rotation (ra, rb) of the slice set and the march offset in (0, 1] (1 = unjittered).
    f32 ra = 1.f;
    f32 rb = 0.f;
    f32 offset = 1.f;
    if (params.jitter) {
        const u32 seed = (x * 73856093U) ^ (y * 19349663U) ^ (params.frame * 83492791U);
        const f32 u = hash_unit(seed) - 0.5f;
        const f32 v = hash_unit(seed ^ 0x9e3779b9U) - 0.5f;
        const f32 len = std::sqrt(u * u + v * v);
        if (len > 1e-4f) {
            ra = u / len;
            rb = v / len;
        }
        offset = 1.f - hash_unit(seed ^ 0x85ebca6bU);
    }

    f32 numerator = 0.f;
    f32 denominator = 0.f;
    for (u32 k = 0u; k < params.slices; ++k) {
        const f32 c = prm.slice_cos[k] * ra - prm.slice_sin[k] * rb;
        const f32 s = prm.slice_sin[k] * ra + prm.slice_cos[k] * rb;
        const math::Vec3 d = viewT * c + viewB * s;
        // Screen direction of d at p (derivative of the projection, up to the positive factor 1 / z^2).
        f32 sdx = cam.fx * (d.x * p.z - p.x * d.z);
        f32 sdy = cam.fy * (d.y * p.z - p.y * d.z);
        const f32 slen = std::sqrt(sdx * sdx + sdy * sdy);
        if (slen < 1e-20f) {
            continue;
        }
        sdx = sdx / slen;
        sdy = sdy / slen;

        const math::Vec3 axis = math::cross(d, viewVec).normalized();
        const math::Vec3 projN = n - axis * n.dot(axis);
        const f32 projNLen = projN.length();
        if (projNLen < 1e-6f) {
            continue;
        }
        const f32 signN = d.dot(projN) >= 0.f ? 1.f : -1.f;
        const f32 cosN = saturate(projN.dot(viewVec) / projNLen);
        const f32 gamma = signN * acos_poly(cosN);
        const f32 sinG = signN * std::sqrt(std::max(0.f, 1.f - cosN * cosN));

        // Horizons start at the tangent plane: cos(gamma + pi/2) = -sin(gamma) on the +d side, +sin on the other.
        const f32 lowCos[2] = {-sinG, sinG};
        f32 horizonCos[2] = {lowCos[0], lowCos[1]};
        for (u32 side = 0u; side < 2u; ++side) {
            const f32 dirX = side == 0u ? sdx : -sdx;
            const f32 dirY = side == 0u ? sdy : -sdy;
            i32 lastX = static_cast<i32>(x);
            i32 lastY = static_cast<i32>(y);
            for (u32 st = 1u; st <= steps; ++st) {
                const f32 t = radiusPx * (static_cast<f32>(st - 1u) + offset) / static_cast<f32>(steps);
                const f32 sx = cxp + dirX * t;
                const f32 sy = cyp + dirY * t;
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
                if (hn.dot(n) <= params.bias) {
                    continue;
                }
                f32 hc = hn.dot(viewVec);
                if (params.falloff > 0.f) {
                    const f32 w = saturate((params.radius - dist) / (params.falloff * params.radius));
                    hc = lowCos[side] + (hc - lowCos[side]) * w;
                }
                horizonCos[side] = std::max(horizonCos[side], hc);
            }
        }

        // A side no sample raised keeps the exact tangent-plane horizon (a plane gives exactly 1).
        f32 h1 = gamma + kHalfPi;
        f32 h0 = gamma - kHalfPi;
        if (horizonCos[0] > lowCos[0]) {
            h1 = gamma + std::max(-kHalfPi, std::min(kHalfPi, acos_poly(horizonCos[0]) - gamma));
        }
        if (horizonCos[1] > lowCos[1]) {
            h0 = gamma + std::max(-kHalfPi, std::min(kHalfPi, -acos_poly(horizonCos[1]) - gamma));
        }
        numerator += projNLen * (arc(h0, gamma, cosN, sinG) + arc(h1, gamma, cosN, sinG));
        denominator += projNLen * (arc(gamma - kHalfPi, gamma, cosN, sinG) + arc(gamma + kHalfPi, gamma, cosN, sinG));
    }
    if (!(denominator > 0.f)) {
        return 1.f;
    }
    const f32 visibility = saturate(numerator / denominator);
    return params.strength == 1.f ? visibility : std::pow(visibility, params.strength);
}

/// One pixel: GTAO visibility (1 for sky).
struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        p.visibility_out[p.view.index(x, y)] = pixel_visibility(p, x, y);
    }
};

} // namespace gtao_kernel
} // namespace fuse::ssfx
