#pragma once

// WP-2.2 area lights and BRDF look-up tables: the single-source math (docs/compute-kernels.md) shared by
// the CPU references (lighting_gpu::ShadeKernel) and mirrored expression for expression by
// shaders/lighting/lc_ltc.{glsl,slang}. Device-safe.
//
// One f32 LUT buffer (BrdfLut, ltc_lut.hpp) holds three tables, all sampled with the same manual
// bilinear filter (texel centres on the grid points, clamped), so the CPU and the GPU read identical
// words and interpolate with identical expressions:
//
//   LTC     64 x 64 x 4  M^-1 of the linearly transformed cosine fitted to FUSE's GGX lobe
//                        (ltc_lut_data.inc, fitted by src/lighting/ltc/ltc_fit.cpp): row = perceptual
//                        roughness (clamped to 0.045), column t = sqrt(1 - N.V)
//   DFG     64 x 64 x 2  (A, B): integral of f cos = F0 A + B (Schlick), same axes; baked at start-up
//                        by DfgBakeKernel ("brdf_lut.dfg", VNDF importance sampling, 1024 Hammersley
//                        samples per texel)
//   sphere  64 x 64 x 1  horizon-clipped form factor of a spherical cap / its unclipped form factor:
//                        row = unclipped form factor sin^2(half angle), column = (cos(elevation) + 1) / 2
//                        (SphereBakeKernel, "brdf_lut.sphere", exact in the azimuth + 512-step quadrature;
//                        the diffuse of large sun disks across the horizon)
//
// Area lights (Heitz, Dupuy, Hill, Neubelt, "Real-Time Polygonal-Light Shading with Linearly
// Transformed Cosines", SIGGRAPH 2016; disks: Heitz and Hill, "Real-Time Line- and Disk-Light Shading
// with Linearly Transformed Cosines", SIGGRAPH 2017 course):
//   rect    exact: the quad in the LTC space is clipped to the horizon and integrated edge by edge
//           (each edge term from the vertex and the edge vector, so small far lights do not cancel);
//   disk    the ellipse in the LTC space (Heitz and Hill 2017 transform the disk the same way): above
//           the horizon the vector form factor as a boundary integral (32-point trapezoid rule of a
//           periodic analytic integrand: exponentially convergent, well conditioned, where the paper's
//           closed-form eigen-frame loses precision in f32); across the horizon an area-preserving
//           32-gon, clipped and integrated like the rectangle, blended into the former over a band;
//   sun     a directional light with an angular radius is a disk at infinity (same code path).
// Outgoing radiance = radiance x [specular albedo x FF(M^-1) + diffuse weight x FF(identity)], with the
// multi-scatter terms of brdf::multi_scatter_terms, so an area light that fills the hemisphere shades
// like the white furnace.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/material/brdf.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::ltc {

// ---------------------------------------------------------------------------------------------
// Light types and GpuLight field use (gpu_scene::GpuLight.type; 1..3 are gpu_scene::GpuLightType)
// ---------------------------------------------------------------------------------------------

/// One-sided rectangle: position = centre, direction = unit normal (the lit side), cosInner / cosOuter =
/// half width / half height, flags = the width axis (tangent), octahedral snorm16x2 (encode_tangent);
/// range = influence radius around the centre; colour x intensity = emitted radiance.
inline constexpr u32 kLightRect = 4u;
/// One-sided disk / ellipse: position = centre, direction = unit normal, cosInner / cosOuter = the radii
/// along the tangent (flags, as the rectangle) and along normal x tangent (equal for a disk); range,
/// colour and intensity as the rectangle.
inline constexpr u32 kLightDisk = 5u;
/// Directional lights: cosOuter = cos(angular radius) of the source disk (1 = punctual, the WP-2.1
/// behaviour); intensity stays the irradiance at normal incidence.
inline constexpr f32 kMinSunCos = 0.999999f; ///< cosOuter above this: punctual

// ---------------------------------------------------------------------------------------------
// LUT layout (f32 words)
// ---------------------------------------------------------------------------------------------

inline constexpr u32 kLtcSize = 64u;
inline constexpr u32 kDfgSize = 64u;
inline constexpr u32 kSphereSize = 64u;
inline constexpr u32 kLtcOffset = 0u;
inline constexpr u32 kDfgOffset = kLtcOffset + kLtcSize * kLtcSize * 4u;
inline constexpr u32 kSphereOffset = kDfgOffset + kDfgSize * kDfgSize * 2u;
inline constexpr u32 kLutWords = kSphereOffset + kSphereSize * kSphereSize;
inline constexpr u32 kDfgSamples = 1024u;
inline constexpr u32 kSphereSteps = 512u;

inline constexpr f32 kPi = 3.14159265358979f;
inline constexpr f32 kTwoPi = 6.28318530717959f;

FUSE_HOST_DEVICE inline f32 saturate(f32 v) { return std::min(std::max(v, 0.f), 1.f); }

/// Grid coordinate of `x` in [0, 1] on n >= 2 points: lower index and weight of the upper one.
FUSE_HOST_DEVICE inline void lut_axis(f32 x, u32 n, u32& i0, f32& w) {
    const f32 fx = saturate(x) * static_cast<f32>(n - 1u);
    i0 = std::min(static_cast<u32>(fx), n - 2u);
    w = fx - static_cast<f32>(i0);
}

/// Bilinear sample of `comps` words per texel of an n x n table at `offset` (row = y, column = x).
FUSE_HOST_DEVICE inline void lut_bilinear(const f32* lut, u32 offset, u32 n, u32 comps, f32 x, f32 y, f32* out) {
    u32 ix = 0;
    u32 iy = 0;
    f32 wx = 0.f;
    f32 wy = 0.f;
    lut_axis(x, n, ix, wx);
    lut_axis(y, n, iy, wy);
    const u32 t00 = offset + (iy * n + ix) * comps;
    const u32 t10 = t00 + comps;
    const u32 t01 = t00 + n * comps;
    const u32 t11 = t01 + comps;
    for (u32 c = 0; c < comps; ++c) {
        const f32 top = lut[t00 + c] + (lut[t10 + c] - lut[t00 + c]) * wx;
        const f32 bottom = lut[t01 + c] + (lut[t11 + c] - lut[t01 + c]) * wx;
        out[c] = top + (bottom - top) * wy;
    }
}

/// LUT column coordinate for N.V: t = sqrt(1 - N.V).
FUSE_HOST_DEVICE inline f32 lut_view_coord(f32 n_dot_v) { return std::sqrt(saturate(1.f - n_dot_v)); }

/// LUT row coordinate: perceptual roughness clamped like the BRDF.
FUSE_HOST_DEVICE inline f32 lut_roughness(f32 roughness) { return std::clamp(roughness, brdf::kMinRoughness, 1.f); }

/// DFG split (A, B) at (N.V, roughness).
FUSE_HOST_DEVICE inline void sample_dfg(const f32* lut, f32 n_dot_v, f32 roughness, f32& a, f32& b) {
    f32 v[2] = {0.f, 0.f};
    lut_bilinear(lut, kDfgOffset, kDfgSize, 2u, lut_view_coord(n_dot_v), lut_roughness(roughness), v);
    a = v[0];
    b = v[1];
}

/// M^-1 = [[m[0], 0, m[1]], [0, 1, 0], [m[2], 0, m[3]]] at (N.V, roughness).
FUSE_HOST_DEVICE inline void sample_ltc(const f32* lut, f32 n_dot_v, f32 roughness, f32* m) {
    lut_bilinear(lut, kLtcOffset, kLtcSize, 4u, lut_view_coord(n_dot_v), lut_roughness(roughness), m);
}

/// Horizon-clipped cap: clipped / unclipped form factor for a cap of unclipped form factor `ff`
/// whose axis has cos(elevation) `z` (N-space).
FUSE_HOST_DEVICE inline f32 sample_sphere(const f32* lut, f32 z, f32 ff) {
    f32 v = 0.f;
    lut_bilinear(lut, kSphereOffset, kSphereSize, 1u, z * 0.5f + 0.5f, ff, &v);
    return v;
}

// ---------------------------------------------------------------------------------------------
// Bakes (single-source kernels)
// ---------------------------------------------------------------------------------------------

inline constexpr const char* kDfgBakeName = "brdf_lut.dfg";
inline constexpr const char* kSphereBakeName = "brdf_lut.sphere";

struct BakeParams {
    f32* lut = nullptr; ///< kLutWords
};

FUSE_HOST_DEVICE inline f32 radical_inverse(u32 bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<f32>(bits) * 2.3283064365386963e-10f;
}

/// GGX visible-normal sample (Heitz, "Sampling the GGX Distribution of Visible Normals", JCGT 2018),
/// isotropic alpha, view `v` in the N = +z frame.
FUSE_HOST_DEVICE inline math::Vec3 sample_vndf(const math::Vec3& v, f32 alpha, f32 u1, f32 u2) {
    const math::Vec3 vh = math::Vec3{alpha * v.x, alpha * v.y, v.z}.normalized();
    const f32 lensq = vh.x * vh.x + vh.y * vh.y;
    const math::Vec3 t1 = lensq > 0.f ? math::Vec3{-vh.y, vh.x, 0.f} * (1.f / std::sqrt(lensq)) : math::Vec3{1.f, 0.f, 0.f};
    const math::Vec3 t2{vh.y * t1.z - vh.z * t1.y, vh.z * t1.x - vh.x * t1.z, vh.x * t1.y - vh.y * t1.x};
    const f32 r = std::sqrt(u1);
    const f32 phi = kTwoPi * u2;
    const f32 p1 = r * std::cos(phi);
    f32 p2 = r * std::sin(phi);
    const f32 s = 0.5f * (1.f + vh.z);
    p2 = (1.f - s) * std::sqrt(std::max(1.f - p1 * p1, 0.f)) + s * p2;
    const math::Vec3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(1.f - p1 * p1 - p2 * p2, 0.f));
    return math::Vec3{alpha * nh.x, alpha * nh.y, std::max(nh.z, 0.f)}.normalized();
}

/// One DFG texel per item: grid (kDfgSize, kDfgSize), x = t = sqrt(1 - N.V), y = roughness.
/// Weight of a VNDF sample for f cos with F = 1: 4 N.V V N.L / G1(V) (V = the shader's visibility,
/// including its N.V floor), so A + B converges to the shaded lobe's own albedo.
struct DfgBakeKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BakeParams& p) const {
        const f32 t = static_cast<f32>(idx.global.x) / static_cast<f32>(kDfgSize - 1u);
        const f32 roughness = lut_roughness(static_cast<f32>(idx.global.y) / static_cast<f32>(kDfgSize - 1u));
        const f32 alpha = roughness * roughness;
        const f32 a2 = alpha * alpha;
        const f32 n_dot_v = std::max(1.f - t * t, brdf::kMinNoV);
        const math::Vec3 v{std::sqrt(std::max(1.f - n_dot_v * n_dot_v, 0.f)), 0.f, n_dot_v};
        const f32 g1 = 2.f * n_dot_v / (n_dot_v + std::sqrt(a2 + (1.f - a2) * n_dot_v * n_dot_v));
        f32 sum_a = 0.f;
        f32 sum_b = 0.f;
        for (u32 i = 0; i < kDfgSamples; ++i) {
            const f32 u1 = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(kDfgSamples);
            const f32 u2 = radical_inverse(i);
            const math::Vec3 h = sample_vndf(v, alpha, u1, u2);
            const f32 v_dot_h = v.dot(h);
            const math::Vec3 l = h * (2.f * v_dot_h) - v;
            if (!(l.z > 0.f) || !(v_dot_h > 0.f)) {
                continue;
            }
            const f32 n_dot_l = l.z;
            const f32 gv = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.f - a2) + a2);
            const f32 gl = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.f - a2) + a2);
            const f32 vis = 0.5f / std::max(gv + gl, 1e-5f);
            const f32 w = 4.f * n_dot_v * vis * n_dot_l / g1;
            const f32 f = saturate(1.f - v_dot_h);
            const f32 f2 = f * f;
            const f32 fc = f2 * f2 * f;
            sum_a += w * (1.f - fc);
            sum_b += w * fc;
        }
        const u32 texel = kDfgOffset + (idx.global.y * kDfgSize + idx.global.x) * 2u;
        p.lut[texel] = sum_a / static_cast<f32>(kDfgSamples);
        p.lut[texel + 1u] = sum_b / static_cast<f32>(kDfgSamples);
    }
};

/// One sphere-table texel per item: grid (kSphereSize, kSphereSize), x = (z + 1) / 2, y = ff.
/// Clipped form factor (1/pi) integral over the cap of max(cos, 0): the azimuth integral is exact,
/// the polar one a 512-step midpoint rule (f64 accumulation).
struct SphereBakeKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BakeParams& p) const {
        const f64 z = -1.0 + 2.0 * static_cast<f64>(idx.global.x) / static_cast<f64>(kSphereSize - 1u);
        const f64 ff = static_cast<f64>(idx.global.y) / static_cast<f64>(kSphereSize - 1u);
        f64 value = z > 0.0 ? z : 0.0; // the limit ff -> 0
        if (ff > 0.0) {
            const f64 sigma = std::asin(std::sqrt(std::min(ff, 1.0)));
            const f64 sz = std::sqrt(std::max(1.0 - z * z, 0.0));
            const f64 dt = sigma / static_cast<f64>(kSphereSteps);
            f64 sum = 0.0;
            for (u32 i = 0; i < kSphereSteps; ++i) {
                const f64 t = (static_cast<f64>(i) + 0.5) * dt;
                const f64 a = std::cos(t) * z;
                const f64 b = std::sin(t) * sz;
                f64 azimuth = 0.0;
                if (a >= b) {
                    azimuth = 6.283185307179586 * a;
                } else if (a > -b) {
                    const f64 phi0 = std::acos(-a / b);
                    azimuth = 2.0 * (a * phi0 + b * std::sin(phi0));
                }
                sum += std::sin(t) * azimuth;
            }
            value = sum * dt / 3.141592653589793 / ff;
        }
        p.lut[kSphereOffset + idx.global.y * kSphereSize + idx.global.x] = static_cast<f32>(value);
    }
};

inline kernel::KernelLaunch make_dfg_bake_launch() {
    return kernel::KernelLaunch{kDfgBakeName, kernel::extent2(kDfgSize, kDfgSize), kernel::Dim3{8u, 8u, 1u}};
}
inline kernel::KernelLaunch make_sphere_bake_launch() {
    return kernel::KernelLaunch{kSphereBakeName, kernel::extent2(kSphereSize, kSphereSize), kernel::Dim3{8u, 8u, 1u}};
}

// ---------------------------------------------------------------------------------------------
// LTC integration
// ---------------------------------------------------------------------------------------------

FUSE_HOST_DEVICE inline math::Vec3 cross3(const math::Vec3& a, const math::Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/// Unit vector, `fallback` for zero / non-finite length.
FUSE_HOST_DEVICE inline math::Vec3 safe_unit(const math::Vec3& v, const math::Vec3& fallback) {
    const f32 len2 = v.dot(v);
    if (!(len2 > 0.f) || !std::isfinite(len2)) {
        return fallback;
    }
    const f32 inv = 1.f / std::sqrt(len2);
    return {v.x * inv, v.y * inv, v.z * inv};
}

/// Per-pixel LTC frame: T1 = V projected on the tangent plane, T2 = N x T1, and the M^-1 entries.
struct LtcFrame {
    math::Vec3 t1{1.f, 0.f, 0.f};
    math::Vec3 t2{0.f, 1.f, 0.f};
    math::Vec3 n{0.f, 0.f, 1.f};
    f32 m[4] = {1.f, 0.f, 0.f, 1.f};
};

FUSE_HOST_DEVICE inline LtcFrame make_frame(const math::Vec3& n, const math::Vec3& v) {
    LtcFrame f{};
    f.n = n;
    const f32 n_dot_v = n.dot(v);
    const math::Vec3 tangent = v - n * n_dot_v;
    // V along N: any tangent (the lobe is isotropic there); a fixed axis least aligned with N.
    const math::Vec3 axis = std::fabs(n.x) < 0.5f ? math::Vec3{1.f, 0.f, 0.f} : math::Vec3{0.f, 1.f, 0.f};
    const math::Vec3 fallback = safe_unit(axis - n * n.dot(axis), math::Vec3{1.f, 0.f, 0.f});
    f.t1 = safe_unit(tangent, fallback);
    f.t2 = cross3(n, f.t1);
    return f;
}

/// World vector -> LTC space: (T1, T2, N) rotation, then M^-1 (identity when `identity`).
FUSE_HOST_DEVICE inline math::Vec3 to_ltc(const LtcFrame& f, bool identity, const math::Vec3& w) {
    const f32 x = w.dot(f.t1);
    const f32 y = w.dot(f.t2);
    const f32 z = w.dot(f.n);
    if (identity) {
        return {x, y, z};
    }
    return {f.m[0] * x + f.m[1] * z, y, f.m[2] * x + f.m[3] * z};
}

/// Edge term of the polygon integral from vertex `a` along edge `e` (b = a + e), both relative to the
/// shading point: acos(a.b) (a x b).z / |a x b| / (|a| |b|)-normalised, i.e. theta / sin(theta) times
/// the z of the unit cross product. a x b = a x e, so nearby vertices do not cancel.
FUSE_HOST_DEVICE inline f32 edge_integral(const math::Vec3& a, const math::Vec3& e) {
    const math::Vec3 b = a + e;
    const f32 la = a.length();
    const f32 lb = b.length();
    const f32 inv = 1.f / std::max(la * lb, 1e-30f);
    const math::Vec3 c = cross3(a, e) * inv;
    const f32 s = c.length();
    const f32 d = a.dot(b) * inv;
    const f32 theta = std::atan2(s, d);
    const f32 k = s > 1e-7f ? theta / s : 1.f;
    return c.z * k;
}

/// Clipped (z >= 0) polygon form factor of the quad (a, a + e0, a + e0 + e1, a + e1) relative to the
/// shading point; positive for a quad wound clockwise seen from the shading point.
FUSE_HOST_DEVICE inline f32 quad_form_factor(const math::Vec3& a, const math::Vec3& e0, const math::Vec3& e1) {
    const math::Vec3 p[4] = {a, a + e0, a + e0 + e1, a + e1};
    const math::Vec3 e[4] = {e0, e1, e0 * -1.f, e1 * -1.f};
    const bool above[4] = {p[0].z > 0.f, p[1].z > 0.f, p[2].z > 0.f, p[3].z > 0.f};
    f32 sum = 0.f;
    if (above[0] && above[1] && above[2] && above[3]) {
        for (u32 i = 0; i < 4u; ++i) {
            sum += edge_integral(p[i], e[i]);
        }
    } else {
        // Sutherland-Hodgman against z = 0: at most 5 vertices.
        math::Vec3 q[5];
        u32 count = 0;
        for (u32 i = 0; i < 4u; ++i) {
            const u32 j = (i + 1u) & 3u;
            if (above[i]) {
                q[count++] = p[i];
            }
            if (above[i] != above[j]) {
                const f32 t = p[i].z / (p[i].z - p[j].z);
                math::Vec3 x = p[i] + e[i] * t;
                x.z = 0.f;
                q[count++] = x;
            }
        }
        if (count < 3u) {
            return 0.f;
        }
        for (u32 i = 0; i < count; ++i) {
            const u32 j = i + 1u < count ? i + 1u : 0u;
            sum += edge_integral(q[i], q[j] - q[i]);
        }
    }
    return std::max(sum, 0.f) * (1.f / kTwoPi);
}

/// Form factor of a one-sided rectangle (centre `c` relative to the shading point, half-axes `ex`,
/// `ey` with ex x ey along the lit side's normal) in the frame (identity or the LTC's M^-1).
FUSE_HOST_DEVICE inline f32 rect_form_factor(const LtcFrame& f, bool identity, const math::Vec3& c, const math::Vec3& ex,
                                             const math::Vec3& ey) {
    // Lit side faces the shading point: (ex x ey) . (0 - c) > 0.
    if (!(cross3(ex, ey).dot(c) < 0.f)) {
        return 0.f;
    }
    const math::Vec3 lc = to_ltc(f, identity, c);
    const math::Vec3 lx = to_ltc(f, identity, ex);
    const math::Vec3 ly = to_ltc(f, identity, ey);
    // Corner c - ex - ey, then +2ey, +2ex: clockwise seen from the shading point on the lit side (the
    // edge integrals' positive orientation; M^-1 and the rotation have positive determinants).
    return quad_form_factor(lc - lx - ly, ly * 2.f, lx * 2.f);
}

/// Number of vertices of the polygon that stands in for an ellipse crossing the horizon.
inline constexpr u32 kEllipseVertices = 32u;
/// Trapezoid points of the boundary integral of an ellipse above the horizon.
inline constexpr u32 kConeSamples = 64u;
/// Blend band above the horizon (fraction of the ellipse's z extent) between the polygon and the cone.
inline constexpr f32 kEllipseBand = 0.25f;

/// Clipped (z >= 0) form factor of the ellipse lc + v1 cos t + v2 sin t (conjugate semi-diameters, the
/// lit side's winding as the rectangle's) as an area-preserving inscribed kEllipseVertices-gon: the
/// polygon is clipped on the fly (streaming Sutherland-Hodgman: the arcs above the horizon edge by
/// edge, then the chord along it) and integrated with the same edge terms as quad_form_factor.
FUSE_HOST_DEVICE inline f32 ellipse_clipped_form_factor(const math::Vec3& lc, const math::Vec3& v1, const math::Vec3& v2) {
    // Area-preserving radius scale sqrt(2 pi / (N sin(2 pi / N))) and the rotation step, N = 32.
    const f32 scale = 1.0032221f;
    const f32 step_c = 0.98078528f; // cos(2 pi / 32)
    const f32 step_s = 0.19509032f; // sin(2 pi / 32)
    const math::Vec3 a1 = v1 * scale;
    const math::Vec3 a2 = v2 * scale;
    f32 cs = 1.f;
    f32 sn = 0.f;
    math::Vec3 first = lc + a1;
    math::Vec3 prev = first;
    math::Vec3 exit_point{};
    math::Vec3 entry_point{};
    bool have_exit = false;
    bool have_entry = false;
    f32 sum = 0.f;
    for (u32 k = 1; k <= kEllipseVertices; ++k) {
        // t decreasing: clockwise seen from the shading point (as rect_form_factor's quad).
        const f32 nc = cs * step_c + sn * step_s;
        const f32 ns = sn * step_c - cs * step_s;
        cs = nc;
        sn = ns;
        const math::Vec3 cur = k == kEllipseVertices ? first : lc + a1 * cs + a2 * sn;
        const bool prev_above = prev.z > 0.f;
        const bool cur_above = cur.z > 0.f;
        if (prev_above && cur_above) {
            sum += edge_integral(prev, cur - prev);
        } else if (prev_above != cur_above) {
            const f32 t = prev.z / (prev.z - cur.z);
            math::Vec3 x = prev + (cur - prev) * t;
            x.z = 0.f;
            if (prev_above) {
                sum += edge_integral(prev, x - prev);
                exit_point = x;
                have_exit = true;
            } else {
                sum += edge_integral(x, cur - x);
                entry_point = x;
                have_entry = true;
            }
        }
        prev = cur;
    }
    if (have_exit && have_entry) {
        sum += edge_integral(exit_point, entry_point - exit_point); // the chord along the horizon
    }
    return std::max(sum, 0.f) * (1.f / kTwoPi);
}

/// Form factor of an ellipse entirely above the horizon (lc + v1 cos t + v2 sin t, the lit side's
/// winding): the vector form factor's z as a boundary integral,
///   FF = -(1 / 2 pi) integral over t of (p x p').z / |p|^2,
///   (p x p').z = -sin t (lc x v1).z + cos t (lc x v2).z + (v1 x v2).z,
/// whose integrand is periodic and analytic, so the kConeSamples-point trapezoid rule converges
/// exponentially; no cubic, no atan2, well conditioned for any cone (Heitz and Hill's closed-form
/// eigen-frame loses the small roots of elongated cones in f32). The oscillating part's mean is
/// subtracted analytically (the trapezoid sums of cos t and sin t over a period are 0): with
/// P0 = |lc|^2 + (|v1|^2 + |v2|^2) / 2 and d = P0 - |p|^2 = -2 lc.q - (q.q - (|v1|^2 + |v2|^2) / 2),
/// q = v1 cos t + v2 sin t, the summand is ((cos t k2 - sin t k1) d / P0 + k3) / |p|^2, which has no
/// |lc|^2-sized cancellation for small or far lights.
FUSE_HOST_DEVICE inline f32 cone_form_factor(const math::Vec3& lc, const math::Vec3& v1, const math::Vec3& v2) {
    const f32 step_c = 0.99518473f; // cos(2 pi / 64)
    const f32 step_s = 0.09801714f; // sin(2 pi / 64)
    const f32 k1 = lc.x * v1.y - lc.y * v1.x;
    const f32 k2 = lc.x * v2.y - lc.y * v2.x;
    const f32 k3 = v1.x * v2.y - v1.y * v2.x;
    const f32 lc2 = lc.dot(lc);
    const f32 half = 0.5f * (v1.dot(v1) + v2.dot(v2));
    const f32 inv_p0 = 1.f / (lc2 + half);
    f32 cs = 1.f;
    f32 sn = 0.f;
    f32 sum = 0.f;
    for (u32 k = 0; k < kConeSamples; ++k) {
        const math::Vec3 q = v1 * cs + v2 * sn;
        const f32 lq = lc.dot(q);
        const f32 qq = q.dot(q);
        const f32 p2 = lc2 + 2.f * lq + qq;
        const f32 d = -2.f * lq - (qq - half);
        sum += ((cs * k2 - sn * k1) * d * inv_p0 + k3) / std::max(p2, 1e-30f);
        const f32 nc = cs * step_c - sn * step_s;
        const f32 ns = sn * step_c + cs * step_s;
        cs = nc;
        sn = ns;
    }
    return saturate(-sum * (1.f / static_cast<f32>(kConeSamples)));
}

/// Form factor of a one-sided disk / ellipse (centre `c` relative to the shading point, orthogonal
/// radius vectors `ex`, `ey`, ex x ey along the lit side's normal) in the frame (identity or the LTC's
/// M^-1): above the horizon the boundary integral of cone_form_factor, 0 below it, and across it the
/// clipped polygon of
/// ellipse_clipped_form_factor (instead of the paper's horizon-clipped sphere approximation).
FUSE_HOST_DEVICE inline f32 disk_form_factor(const LtcFrame& f, bool identity, const math::Vec3& c, const math::Vec3& ex,
                                             const math::Vec3& ey) {
    if (!(cross3(ex, ey).dot(c) < 0.f)) {
        return 0.f;
    }
    const math::Vec3 lc = to_ltc(f, identity, c);
    const math::Vec3 e1 = to_ltc(f, identity, ex);
    const math::Vec3 e2 = to_ltc(f, identity, ey);
    // The ellipse's z range: lc.z +- |(e1.z, e2.z)|. Across the horizon the clipped polygon; within
    // kEllipseBand x z_span above it a linear blend polygon -> cone, so the form factor is continuous
    // where the method changes (the 32-gon and the cone differ by ~1e-4).
    const f32 z_span = std::sqrt(e1.z * e1.z + e2.z * e2.z);
    const f32 low = lc.z - z_span;
    if (!(lc.z + z_span > 0.f)) {
        return 0.f;
    }
    const f32 band = kEllipseBand * z_span;
    const f32 polygon = low < band ? ellipse_clipped_form_factor(lc, e1, e2) : 0.f;
    if (!(low > 0.f)) {
        return polygon;
    }
    const f32 cone = cone_form_factor(lc, e1, e2);
    return low < band ? polygon + (cone - polygon) * (low / band) : cone;
}

// ---------------------------------------------------------------------------------------------
// Area-light shading
// ---------------------------------------------------------------------------------------------

/// Signed 16-bit normalised word (bits 0..15) -> [-1, 1].
FUSE_HOST_DEVICE inline f32 snorm16(u32 bits) {
    const u32 w = bits & 0xFFFFu;
    const i32 v = w >= 0x8000u ? static_cast<i32>(w) - 65536 : static_cast<i32>(w);
    return std::max(static_cast<f32>(v) * (1.f / 32767.f), -1.f);
}

/// Octahedral snorm16x2 (x = bits 0..15, y = bits 16..31) -> unit vector.
FUSE_HOST_DEVICE inline math::Vec3 decode_tangent(u32 bits) {
    const f32 ox = snorm16(bits);
    const f32 oy = snorm16(bits >> 16u);
    math::Vec3 n{ox, oy, 1.f - std::fabs(ox) - std::fabs(oy)};
    if (n.z < 0.f) {
        const f32 x = (1.f - std::fabs(oy)) * (ox >= 0.f ? 1.f : -1.f);
        const f32 y = (1.f - std::fabs(ox)) * (oy >= 0.f ? 1.f : -1.f);
        n.x = x;
        n.y = y;
    }
    return safe_unit(n, math::Vec3{1.f, 0.f, 0.f});
}

/// Rectangle / disk half-axes from a light's normal, tangent and half extents: ex along the tangent
/// (orthogonalised), ey = normal x ex, so ex x ey = the normal (the lit side).
FUSE_HOST_DEVICE inline void area_axes(const math::Vec3& normal, const math::Vec3& tangent, f32 half_x, f32 half_y,
                                       math::Vec3& ex, math::Vec3& ey) {
    const math::Vec3 axis = std::fabs(normal.x) < 0.5f ? math::Vec3{1.f, 0.f, 0.f} : math::Vec3{0.f, 1.f, 0.f};
    const math::Vec3 fallback = safe_unit(axis - normal * normal.dot(axis), math::Vec3{1.f, 0.f, 0.f});
    const math::Vec3 t = safe_unit(tangent - normal * normal.dot(tangent), fallback);
    const math::Vec3 b = cross3(normal, t);
    ex = t * half_x;
    ey = b * half_y;
}

/// Outgoing radiance per unit emitted radiance of a rectangle (`disk` false) or disk light:
/// specular albedo x FF(M^-1) + diffuse weight / pi x pi FF(identity) (= diffuse x FF).
FUSE_HOST_DEVICE inline math::Vec3 area_light_response(const LtcFrame& f,
                                                       const brdf::MultiScatterTerms& t, bool disk,
                                                       const math::Vec3& c, const math::Vec3& ex,
                                                       const math::Vec3& ey) {
    const f32 ff_spec = disk ? disk_form_factor(f, false, c, ex, ey) : rect_form_factor(f, false, c, ex, ey);
    const f32 ff_diff = disk ? disk_form_factor(f, true, c, ex, ey) : rect_form_factor(f, true, c, ex, ey);
    return {t.specular.x * ff_spec + t.diffuse.x * ff_diff, t.specular.y * ff_spec + t.diffuse.y * ff_diff,
            t.specular.z * ff_spec + t.diffuse.z * ff_diff};
}

/// LTC distribution value at direction `w` (world, unit): D_o(M^-1 w / |M^-1 w|) |M^-1| / |M^-1 w|^3,
/// D_o = max(z, 0) / pi (the normalised M^-1 has det m0 m3 - m1 m2).
FUSE_HOST_DEVICE inline f32 ltc_density(const LtcFrame& f, const math::Vec3& w) {
    const math::Vec3 l = to_ltc(f, false, w);
    const f32 len2 = l.dot(l);
    if (!(l.z > 0.f) || !(len2 > 0.f)) {
        return 0.f;
    }
    const f32 len = std::sqrt(len2);
    const f32 det = f.m[0] * f.m[3] - f.m[1] * f.m[2];
    return (l.z / len) * (1.f / kPi) * det / (len2 * len);
}

/// Clipped / unclipped form factor of a cap (sine of its angular radius `sin_r`) whose axis has
/// cos(elevation from the normal) `z`: exact z above the horizon, 0 below; across it, small caps
/// (sin_r <= 0.1) use the flat-cap closed form sin_r g(h) / pi, h = z / sin_r,
/// g(h) = h (pi/2 + h sqrt(1 - h^2) + asin h) + 2/3 (1 - h^2)^(3/2) (the clamped linear elevation
/// integrated over the disk; continuous with z at h = 1 and 0 at h = -1), larger ones the sphere table.
FUSE_HOST_DEVICE inline f32 cap_clipped(const f32* lut, f32 z, f32 sin_r) {
    if (z >= sin_r) {
        return z;
    }
    if (z <= -sin_r) {
        return 0.f;
    }
    if (sin_r <= 0.1f) {
        const f32 h = std::min(std::max(z / sin_r, -1.f), 1.f);
        const f32 q = std::sqrt(std::max(1.f - h * h, 0.f));
        const f32 g = h * (0.5f * kPi + h * q + std::asin(h)) + (2.f / 3.f) * q * q * q;
        return sin_r * g * (1.f / kPi);
    }
    return sample_sphere(lut, z, sin_r * sin_r);
}

/// F-free specular lobe D V N.L towards `l` (0 below the horizon).
FUSE_HOST_DEVICE inline f32 lobe_value(const math::Vec3& n, const math::Vec3& v, const math::Vec3& l, f32 roughness) {
    const f32 n_dot_l = n.dot(l);
    if (!(n_dot_l > 0.f)) {
        return 0.f;
    }
    const math::Vec3 h = safe_unit(v + l, n);
    const f32 n_dot_h = std::max(n.dot(h), 0.f);
    const math::Vec3 nxh = cross3(n, h);
    const f32 one_minus_noh2 = n_dot_h > 0.f ? std::min(nxh.dot(nxh), 1.f) : 1.f;
    return brdf::ggx_dv(n_dot_h, one_minus_noh2, std::max(n.dot(v), brdf::kMinNoV), n_dot_l, roughness) * n_dot_l;
}

/// Directional light with an angular radius (cos_radius < kMinSunCos): a disk of radiance
/// irradiance / (pi sin^2) and solid angle omega around `l` (unit). Per unit irradiance:
///   diffuse   diffuse / pi x cap_clipped (exact above the horizon);
///   specular  radiance x [(1 - w) omega f_spec(l) cos + w E_spec FF_ltc(disk)]: the exact BRDF at the
///             centre (the punctual light's, x omega / (pi sin^2) = 2 / (1 + cos)) where the lobe is
///             smooth across the disk, the LTC integral of the disk where it is not:
///             w = max(w_var, w_sharp x w_near): w_var from the lobe's variation across the disk (D V N.L
///             at the disk point closest to the mirror direction R over the centre's: 0 up to 1.5x,
///             1 from 3x), w_sharp = saturate(2 radius / alpha - 1) (lobe narrower than the disk) and
///             w_near = saturate(1 - (angle(l, R) - radius) / (4 alpha)) (the disk on the lobe's peak).
///             Continuous; the LTC is not used for tails (where its shape error and its horizon differ
///             most from the BRDF's).
FUSE_HOST_DEVICE inline math::Vec3 sun_response(const f32* lut, const LtcFrame& f, const brdf::MultiScatterTerms& t,
                                                const math::Vec3& albedo, f32 roughness, const math::Vec3& v,
                                                const math::Vec3& l, f32 cos_radius) {
    const f32 c = std::min(std::max(cos_radius, -1.f), 1.f);
    const f32 sin2 = std::max(1.f - c * c, 1e-12f);
    const f32 sin_r = std::sqrt(sin2);
    const f32 radius = std::acos(c);
    const f32 radiance = 1.f / (kPi * sin2);
    const f32 omega = kTwoPi * (1.f - c);
    const f32 r = lut_roughness(roughness);
    const f32 alpha = r * r;
    const f32 n_dot_v = f.n.dot(v);
    const math::Vec3 refl = f.n * (2.f * n_dot_v) - v;
    const f32 cos_angle = std::min(std::max(refl.dot(l), -1.f), 1.f);
    const f32 angle = std::acos(cos_angle);
    // Lobe variation across the disk: F-free D V N.L at the centre and at the disk point closest to R.
    const math::Vec3 toward = safe_unit(refl - l * cos_angle, l);
    const math::Vec3 near = angle <= radius ? refl : l * c + toward * sin_r;
    const f32 lobe_c = lobe_value(f.n, v, l, roughness);
    const f32 lobe_p = lobe_value(f.n, v, near, roughness);
    const f32 w_var = lobe_p > 0.f ? (lobe_c > 0.f ? saturate((lobe_p / lobe_c - 1.5f) * (1.f / 1.5f)) : 1.f) : 0.f;
    const f32 w_centre = saturate(2.f * radius / alpha - 1.f) * saturate(1.f - (angle - radius) / (4.f * alpha));
    const f32 w = std::max(w_var, w_centre);
    f32 ff = 0.f;
    if (w > 0.f) {
        const f32 tan_r = sin_r / std::max(c, 1e-6f);
        const math::Vec3 axis = std::fabs(l.x) < 0.5f ? math::Vec3{1.f, 0.f, 0.f} : math::Vec3{0.f, 1.f, 0.f};
        const math::Vec3 u = safe_unit(axis - l * l.dot(axis), math::Vec3{1.f, 0.f, 0.f});
        const math::Vec3 ex = u * tan_r;
        const math::Vec3 ey = cross3(u, l) * tan_r; // ex x ey = -l: the lit side faces the shading point
        ff = disk_form_factor(f, false, l, ex, ey);
    }
    brdf::MultiScatterTerms spec_only = t;
    spec_only.diffuse = {};
    const math::Vec3 exact = brdf::multi_scatter_cos(spec_only, albedo, roughness, f.n, v, l);
    const f32 k_exact = radiance * omega * (1.f - w);
    const f32 k_ltc = radiance * ff * w;
    const f32 diff = cap_clipped(lut, f.n.dot(l), sin_r) * (1.f / kPi);
    return {exact.x * k_exact + t.specular.x * k_ltc + t.diffuse.x * diff,
            exact.y * k_exact + t.specular.y * k_ltc + t.diffuse.y * diff,
            exact.z * k_exact + t.specular.z * k_ltc + t.diffuse.z * diff};
}

/// Influence window of an area light (the point light's window without the inverse square):
/// (1 - (d / range)^4)^2, exactly 0 at d >= range.
FUSE_HOST_DEVICE inline f32 area_window(f32 distance, f32 range) {
    if (!(range > 0.f) || !(distance < range)) {
        return 0.f;
    }
    const f32 ratio = distance / range;
    const f32 ratio2 = ratio * ratio;
    const f32 w = saturate(1.f - ratio2 * ratio2);
    return w * w;
}

} // namespace fuse::renderer::ltc
