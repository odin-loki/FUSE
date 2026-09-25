#pragma once

// WP-7.2 ReSTIR DI and GI: the single-source kernel. Every compute kernel (shaders/restir/restir_common.{glsl,
// slang} + the restir_*.{comp,slang} entry points) is a line-for-line twin of the functions below, so a GPU pass
// equals this code bit for bit on the same inputs (fuse_rp_restir_vk_passes_* checks every pass on the GPU's
// own read-back inputs). The CPU runner (RestirCpu, restir_reference.hpp) runs the same functions.
//
// Papers (self-written from them):
//   [ReSTIR]    B. Bitterli, C. Wyman, M. Pharr, P. Shirley, A. Lefohn, W. Jarosz. "Spatiotemporal reservoir
//               resampling for real-time ray tracing with dynamic direct lighting". ACM TOG 39(4), 2020.
//   [ReSTIR GI] Y. Ouyang, S. Liu, M. Kettunen, M. Pharr, J. Pantaleoni. "ReSTIR GI: Path Resampling for
//               Real-Time Path Tracing". Computer Graphics Forum 40(8), 2021.
//   [GRIS]      D. Lin, M. Kettunen, B. Bitterli, J. Pantaleoni, C. Yuksel, C. Wyman. "Generalized Resampled
//               Importance Sampling: Foundations of ReSTIR". ACM TOG 41(4), 2022 (the unbiasedness conditions,
//               contribution weights, Talbot / generalized-balance MIS weights with confidence M).
//
// Integrands (Lambert at the visible point x with normal n; outputs are demodulated, i.e. divided by albedo):
//   DI  f(y) = Le(y) G(x, y) V(x, y) / pi over the light-tree emitters, y = (light, u1, u2) -> a point on the
//       light (area measure) or the light itself (point / spot / directional: counting measure);
//       target p-hat(y) = luminance(Le G) (unshadowed; x V in the unbiased mode's reuse)
//   GI  f(x_s) = Lo(x_s) cos(theta_x) V / pi over solid angle at x, x_s the secondary vertex (ray-query hit),
//       Lo(x_s) = one next-event estimate at x_s (albedo_s / pi x Le G V / p over one light-tree sample);
//       target p-hat = luminance(Lo) cos(theta_x); reuse moves samples between visible points with the
//       reconnection Jacobian |d omega_q / d omega_r| = (cos phi_r / |x_r - x_s|^2) / (cos phi_q / |x_q - x_s|^2)
//
// Reuse (both chains, per pixel):
//   initial   DI: RIS over diCandidates light-tree samples (source pdf = selection pmf x area density),
//             visibility reuse (W = 0 when the chosen candidate is occluded); GI: one cosine-weighted ray
//   temporal  the canonical reservoir + the previous frame's final reservoir at the reprojected pixel
//             (WP-4.1 UV motion), history M capped at mCap x canonical M
//   spatial   the canonical reservoir + up to N neighbours in a disk of `radius` pixels (normal / depth
//             similarity test), any number of iterations
//   combine   biased mode ([ReSTIR] Alg. 4, "1 / M"): w_i = p-hat_r(y_i) W_i M_i (J_i), W = sum w / (p-hat(Y) sum M)
//             unbiased mode ([GRIS] generalized balance heuristic with confidences): w_i = m_i(y_i) p-hat_r(y_i)
//             W_i (J_i), m_i(y) = M_i p-hat_i(y) / sum_j M_j p-hat_j(y) with every p-hat_j VISIBILITY-TESTED from
//             domain j's own surface (GI: p-hat_j in area measure at x_s), W = sum w / p-hat_r(Y)
//   shade     DI: Le G V W / pi; GI: Lo cos V W / pi (demodulated), final reservoirs -> history
//
// Bit-exactness rules (as light_tree_kernel.hpp): only IEEE-exact operations (+ - * /, sqrt, abs, comparisons,
// int <-> float conversions), min / max written as comparisons, fixed evaluation order (dot products left to
// right, explicit temporaries), no transcendental function (the disk / hemisphere maps use lt_concentric's fixed
// polynomials), no contraction (-ffp-contract=off inherited from fuse_light_tree, GLSL `precise`, Slang
// -fp-mode precise). Random numbers: integer hashes (PCG output of a per-pixel seed) and the WP-6.2
// Owen-scrambled Sobol sequence (rtfxSample2) continued across frames.
//
// The environment `Env` supplies buffers and rays (the GPU twins read buffer device addresses and run ray
// queries):
//   RestirSurfaceF surface(u32 slot, u32 pixel)    slot 0 = this frame, 1 = previous frame
//   RestirDiReservoir diSource(u32 pixel) / diHistory(u32 pixel), RestirGiReservoir giSource / giHistory
//   void motion(u32 pixel, f32& mx, f32& my)       UV motion (called only with kRestirFlagMotion)
//   bool occluded(const RV3& o, const RV3& d, f32 tMax)
//   bool traceHit(const RV3& o, const RV3& d, f32 tMin, f32 tMax, RestirHitF& hit)  closest hit: t, geometric
//                                                   normal (any length / side), albedo
//   const light_tree::LightTreeView& tree(), const RestirLight& light(u32 index)

#include <fuse/renderer/light_tree/light_tree_kernel.hpp>
#include <fuse/renderer/restir/restir_types.hpp>
#include <fuse/renderer/rt_effects/rt_effects_kernel.hpp>
#include <fuse/types.hpp>

#include <cmath>

namespace fuse::renderer::restir {

using light_tree::LightTreeSample;
using light_tree::LightTreeView;

// --- vectors ---------------------------------------------------------------------------------------------
struct RV3 {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
};

inline RV3 rsLoad(const f32 v[3]) { return RV3{v[0], v[1], v[2]}; }
inline void rsStore(const RV3& a, f32 v[3]) {
    v[0] = a.x;
    v[1] = a.y;
    v[2] = a.z;
}
inline RV3 rsAdd(const RV3& a, const RV3& b) { return RV3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline RV3 rsSub(const RV3& a, const RV3& b) { return RV3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline RV3 rsScale(const RV3& a, f32 s) { return RV3{a.x * s, a.y * s, a.z * s}; }
inline RV3 rsNeg(const RV3& a) { return RV3{-a.x, -a.y, -a.z}; }
inline f32 rsDot(const RV3& a, const RV3& b) {
    const f32 x = a.x * b.x;
    const f32 y = a.y * b.y;
    const f32 z = a.z * b.z;
    const f32 xy = x + y;
    return xy + z;
}
inline f32 rsLength(const RV3& a) { return std::sqrt(rsDot(a, a)); }
inline RV3 rsNormalize(const RV3& a, const RV3& fallback) {
    const f32 l = rsLength(a);
    if (!(l > 1e-20f)) {
        return fallback;
    }
    return RV3{a.x / l, a.y / l, a.z / l};
}
inline RV3 rsCross(const RV3& a, const RV3& b) {
    const f32 x0 = a.y * b.z;
    const f32 x1 = a.z * b.y;
    const f32 y0 = a.z * b.x;
    const f32 y1 = a.x * b.z;
    const f32 z0 = a.x * b.y;
    const f32 z1 = a.y * b.x;
    return RV3{x0 - x1, y0 - y1, z0 - z1};
}
inline f32 rsLum(const RV3& c) {
    const f32 r = c.x * 0.2126f;
    const f32 g = c.y * 0.7152f;
    const f32 b = c.z * 0.0722f;
    const f32 rg = r + g;
    return rg + b;
}

/// Orthonormal basis around a unit n (Duff et al. 2017), fixed evaluation order.
inline void rsBasis(const RV3& n, RV3& t, RV3& b) {
    const f32 sign = n.z >= 0.f ? 1.f : -1.f;
    const f32 a = -1.f / (sign + n.z);
    const f32 c = (n.x * n.y) * a;
    const f32 txx = ((sign * n.x) * n.x) * a;
    t = RV3{1.f + txx, sign * c, -(sign * n.x)};
    const f32 byy = (n.y * n.y) * a;
    b = RV3{c, sign + byy, -n.y};
}

// --- random numbers ----------------------------------------------------------------------------------------
inline constexpr u32 kRsStreamDiSobol = 0x100u;
inline constexpr u32 kRsStreamDiInitial = 0x101u;
inline constexpr u32 kRsStreamDiReuse = 0x110u;  ///< + mode * 16 + iteration
inline constexpr u32 kRsStreamGiDir = 0x200u;
inline constexpr u32 kRsStreamGiNee = 0x201u;
inline constexpr u32 kRsStreamGiNeeSobol = 0x202u;
inline constexpr u32 kRsStreamGiReuse = 0x210u;  ///< + mode * 16 + iteration

/// Per-(pixel, frame, stream, run) seed (the WP-6.2 lowbias32 hash chain).
inline u32 rsSeed(u32 px, u32 py, u32 frame, u32 stream, u32 seed) {
    using rt_effects::rtfxHash;
    return rtfxHash(px + rtfxHash(py + rtfxHash(frame + rtfxHash(stream + rtfxHash(seed)))));
}

/// PCG-RXS-M-XS step: the next uniform in [0, 1) (exact 24-bit float).
inline f32 rsNext(u32& state) {
    state = state * 747796405u + 2891336453u;
    u32 w = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    w = (w >> 22u) ^ w;
    return rt_effects::rtfxUnit(w);
}

/// Point n of the pixel's Owen-scrambled Sobol sequence `seed` (WP-6.2).
inline void rsSobol(u32 n, u32 seed, f32& u, f32& v) {
    u32 ux = 0u;
    u32 uy = 0u;
    rt_effects::rtfxSample2(n, seed, ux, uy);
    u = rt_effects::rtfxUnit(ux);
    v = rt_effects::rtfxUnit(uy);
}

// --- surfaces ----------------------------------------------------------------------------------------------
/// A visible point (one pixel of the surface buffer). depth = linear view depth, 0 = no surface.
struct RestirSurfaceF {
    RV3 p{};
    f32 depth = 0.f;
    RV3 n{};
};

inline bool rsValid(const RestirSurfaceF& s) { return s.depth > 0.f; }

/// Closest hit of a GI ray, as the environment reports it.
struct RestirHitF {
    f32 t = -1.f;
    RV3 normal{}; ///< geometric normal, any length or side
    RV3 albedo{};
    u32 instance = kRestirInvalid;
    u32 primitive = kRestirInvalid;
};

/// Shadow-ray origin above a surface point: p + n (normalBias + viewBias |p - camera|).
inline RV3 rsOffset(const RestirFrameConstants& c, const RV3& p, const RV3& n) {
    const RV3 toCamera = rsSub(p, rsLoad(c.cameraPosition));
    const f32 vb = c.viewBias * rsLength(toCamera);
    const f32 bias = c.normalBias + vb;
    return rsAdd(p, rsScale(n, bias));
}

/// Signed-octahedral RT0 normal (GBufferEncoding::encodeNormalRgba16f).
inline RV3 rsOctDecode(f32 ox, f32 oy) {
    const f32 ax = std::fabs(ox);
    const f32 ay = std::fabs(oy);
    const f32 z0 = 1.f - ax;
    RV3 n{ox, oy, z0 - ay};
    if (n.z < 0.f) {
        const f32 x = (1.f - ay) * (ox >= 0.f ? 1.f : -1.f);
        const f32 y = (1.f - ax) * (oy >= 0.f ? 1.f : -1.f);
        n.x = x;
        n.y = y;
    }
    return rsNormalize(n, RV3{0.f, 0.f, 1.f});
}

/// restir.prepare: the G-buffer texels of pixel (px, py) -> the surface record (posDepth, normal).
/// depth = RT4 (forward z/w, >= 1 = sky), (ox, oy) = RT0.xy. False (all zero) without a surface.
inline bool rsPrepare(const RestirFrameConstants& c, u32 px, u32 py, f32 depth, f32 ox, f32 oy, RestirSurfaceF& out) {
    out = RestirSurfaceF{};
    if (!(depth < 1.f) || !(depth >= 0.f)) {
        return false;
    }
    const f32 nx = (((static_cast<f32>(px) + 0.5f) * c.invWidth) * 2.f) - 1.f;
    const f32 ny = (((static_cast<f32>(py) + 0.5f) * c.invHeight) * 2.f) - 1.f;
    const f32* m = c.invViewProj;
    const f32 x = ((m[0] * nx + m[4] * ny) + m[8] * depth) + m[12];
    const f32 y = ((m[1] * nx + m[5] * ny) + m[9] * depth) + m[13];
    const f32 z = ((m[2] * nx + m[6] * ny) + m[10] * depth) + m[14];
    const f32 w = ((m[3] * nx + m[7] * ny) + m[11] * depth) + m[15];
    const RV3 p{x / w, y / w, z / w};
    const f32 lin = rsDot(rsSub(p, rsLoad(c.cameraPosition)), rsLoad(c.cameraForward));
    if (!(lin > 0.f)) {
        return false;
    }
    out.p = p;
    out.depth = lin;
    out.n = rsOctDecode(ox, oy);
    return true;
}

// --- DI: light samples ---------------------------------------------------------------------------------------
/// Everything about one light sample at one surface.
struct RsLightEval {
    RV3 contrib{};  ///< Le G (unshadowed): the demodulated integrand without V / pi
    f32 phat = 0.f; ///< luminance(contrib)
    RV3 origin{};   ///< shadow ray
    RV3 dir{};
    f32 tMax = 0.f; ///< 0: nothing to trace (the light touches the origin)
};

inline bool rsAreaKind(u32 kind) {
    return kind == light_tree::kLtKindTriangle || kind == light_tree::kLtKindRect || kind == light_tree::kLtKindDisk;
}

/// Source density of a light-tree sample: selection pmf x area density (area kinds), pmf (delta kinds).
inline f32 rsSourcePdf(u32 kind, f32 pmf, f32 pdfArea) { return rsAreaKind(kind) ? pmf * pdfArea : pmf; }

/// Spot cone falloff (smoothstep between cosOuter and cosInner).
inline f32 rsSpot(f32 cosA, f32 cosInner, f32 cosOuter) {
    if (cosInner > cosOuter) {
        const f32 range = cosInner - cosOuter;
        f32 t = (cosA - cosOuter) / range;
        t = t < 0.f ? 0.f : t;
        t = t > 1.f ? 1.f : t;
        const f32 tt = t * t;
        const f32 k = 3.f - 2.f * t;
        return tt * k;
    }
    return cosA >= cosOuter ? 1.f : 0.f;
}

/// Evaluates sample (light, u1, u2) at surface s. False (e zeroed) when it contributes nothing.
template <typename Env>
inline bool rsEvalLight(const Env& env, const RestirFrameConstants& c, const RestirSurfaceF& s, u32 light, f32 u1, f32 u2,
                        RsLightEval& e) {
    e = RsLightEval{};
    const LightTreeView& tree = env.tree();
    if (light >= tree.emitterCount || light >= c.lightCount) {
        return false;
    }
    const light_tree::LightTreeEmitter& em = tree.emitters[light];
    const RestirLight& L = env.light(light);
    f32 pos[3] = {0.f, 0.f, 0.f};
    f32 pdfArea = 0.f;
    light_tree::lt_sample_point(em, u1, u2, pos, pdfArea);
    const RV3 radiance = rsLoad(L.radiance);
    e.origin = rsOffset(c, s.p, s.n);
    if (em.kind == light_tree::kLtKindDirectional) {
        const RV3 wi{-pos[0], -pos[1], -pos[2]};
        const f32 cosX = rsDot(s.n, wi);
        if (!(cosX > 0.f)) {
            return false;
        }
        e.contrib = rsScale(radiance, cosX);
        e.dir = wi;
        e.tMax = c.farDistance;
    } else {
        const RV3 target = rsLoad(pos);
        const RV3 d = rsSub(target, s.p);
        const f32 dist2 = rsDot(d, d);
        if (!(dist2 > 1e-12f)) {
            return false;
        }
        const f32 dist = std::sqrt(dist2);
        const RV3 wi{d.x / dist, d.y / dist, d.z / dist};
        const f32 cosX = rsDot(s.n, wi);
        if (!(cosX > 0.f)) {
            return false;
        }
        f32 g = 0.f;
        if (rsAreaKind(em.kind)) {
            f32 cosL = -rsDot(rsLoad(em.normal), wi);
            if ((em.flags & light_tree::kLtFlagTwoSided) != 0u) {
                cosL = std::fabs(cosL);
            }
            if (!(cosL > 0.f)) {
                return false;
            }
            g = (cosX * cosL) / dist2;
        } else {
            g = cosX / dist2;
            if (em.kind == light_tree::kLtKindSpot) {
                const f32 cosA = -rsDot(rsLoad(em.normal), wi);
                g = g * rsSpot(cosA, L.cosInner, L.cosOuter);
            }
        }
        e.contrib = rsScale(radiance, g);
        const RV3 toLight = rsSub(target, e.origin);
        const f32 dl = rsLength(toLight);
        if (dl > 1e-6f) {
            e.dir = RV3{toLight.x / dl, toLight.y / dl, toLight.z / dl};
            e.tMax = dl * kRestirShadowShorten;
        } else {
            e.dir = s.n;
            e.tMax = 0.f;
        }
    }
    e.phat = rsLum(e.contrib);
    return e.phat > 0.f;
}

/// Target of a DI sample at s (optionally visibility-tested).
template <typename Env>
inline f32 rsDiTarget(Env& env, const RestirFrameConstants& c, const RestirSurfaceF& s, const RestirDiReservoir& r,
                      bool visibility) {
    RsLightEval e{};
    if (!rsValid(s) || r.light == kRestirInvalid || !rsEvalLight(env, c, s, r.light, r.u1, r.u2, e)) {
        return 0.f;
    }
    if (visibility && e.tMax > 0.f && env.occluded(e.origin, e.dir, e.tMax)) {
        return 0.f;
    }
    return e.phat;
}

/// restir.di.initial: RIS over the light tree + visibility reuse.
template <typename Env>
inline RestirDiReservoir rsDiInitial(Env& env, const RestirFrameConstants& c, u32 px, u32 py) {
    const u32 pixel = py * c.width + px;
    RestirDiReservoir r{};
    const RestirSurfaceF s = env.surface(0u, pixel);
    if (!rsValid(s)) {
        return r;
    }
    r.M = 1.f;
    const u32 candidates = c.diCandidates;
    if (candidates == 0u) {
        return r;
    }
    const u32 sobol = rsSeed(px, py, 0u, kRsStreamDiSobol, c.seed);
    u32 rng = rsSeed(px, py, c.frameIndex, kRsStreamDiInitial, c.seed);
    const f32 p[3] = {s.p.x, s.p.y, s.p.z};
    const f32 n[3] = {s.n.x, s.n.y, s.n.z};
    f32 wSum = 0.f;
    f32 selPhat = 0.f;
    RsLightEval sel{};
    for (u32 j = 0; j < candidates; ++j) {
        const f32 u0 = rsNext(rng);
        f32 u1 = 0.f;
        f32 u2 = 0.f;
        rsSobol(c.frameIndex * candidates + j, sobol, u1, u2);
        const LightTreeSample ls = light_tree::lt_sample(env.tree(), p, n, u0, u1, u2);
        if (ls.light == light_tree::kLtInvalid) {
            continue;
        }
        const f32 src = rsSourcePdf(ls.kind, ls.pmf, ls.pdfArea);
        if (!(src > 0.f)) {
            continue;
        }
        RsLightEval e{};
        if (!rsEvalLight(env, c, s, ls.light, u1, u2, e)) {
            continue;
        }
        const f32 w = e.phat / src;
        wSum = wSum + w;
        const f32 u = rsNext(rng);
        if (u * wSum < w) {
            r.light = ls.light;
            r.u1 = u1;
            r.u2 = u2;
            selPhat = e.phat;
            sel = e;
        }
    }
    if (r.light != kRestirInvalid) {
        const f32 mean = wSum / static_cast<f32>(candidates);
        r.W = mean / selPhat;
        r.targetPdf = selPhat;
        if ((c.flags & kRestirFlagVisibilityReuse) != 0u && sel.tMax > 0.f && env.occluded(sel.origin, sel.dir, sel.tMax)) {
            r.W = 0.f;
        }
    }
    return r;
}

// --- reuse helpers --------------------------------------------------------------------------------------------
/// Neighbour accepted for reuse (it does not depend on any sample, so both modes may use it).
inline bool rsSimilar(const RestirFrameConstants& c, const RestirSurfaceF& s, const RestirSurfaceF& q) {
    if (!rsValid(q)) {
        return false;
    }
    if (!(rsDot(s.n, q.n) >= c.normalThreshold)) {
        return false;
    }
    const f32 dz = std::fabs(q.depth - s.depth);
    return dz <= c.depthThreshold * s.depth;
}

/// Previous-frame pixel of (px, py) through the UV motion (nearest). False when off screen / no history.
template <typename Env>
inline bool rsReproject(const Env& env, const RestirFrameConstants& c, u32 px, u32 py, u32& q) {
    if ((c.flags & kRestirFlagHistory) == 0u) {
        return false;
    }
    f32 mx = 0.f;
    f32 my = 0.f;
    if ((c.flags & kRestirFlagMotion) != 0u) {
        env.motion(py * c.width + px, mx, my);
    }
    const f32 ux = ((static_cast<f32>(px) + 0.5f) * c.invWidth) - mx;
    const f32 uy = ((static_cast<f32>(py) + 0.5f) * c.invHeight) - my;
    if (!(ux >= 0.f && ux < 1.f && uy >= 0.f && uy < 1.f)) {
        return false;
    }
    u32 qx = static_cast<u32>(ux * static_cast<f32>(c.width));
    u32 qy = static_cast<u32>(uy * static_cast<f32>(c.height));
    qx = qx < c.width ? qx : c.width - 1u;
    qy = qy < c.height ? qy : c.height - 1u;
    q = qy * c.width + qx;
    return true;
}

/// A random neighbour in the disk of `radius` pixels (always draws two numbers). False for the pixel itself or
/// off screen.
inline bool rsNeighbor(const RestirFrameConstants& c, u32 px, u32 py, f32 radius, u32& rng, u32& q) {
    const f32 a = rsNext(rng);
    const f32 b = rsNext(rng);
    f32 ox = 0.f;
    f32 oy = 0.f;
    light_tree::lt_concentric(a, b, ox, oy);
    const s32 dx = static_cast<s32>(ox * radius);
    const s32 dy = static_cast<s32>(oy * radius);
    if (dx == 0 && dy == 0) {
        return false;
    }
    const s32 qx = static_cast<s32>(px) + dx;
    const s32 qy = static_cast<s32>(py) + dy;
    if (qx < 0 || qy < 0 || qx >= static_cast<s32>(c.width) || qy >= static_cast<s32>(c.height)) {
        return false;
    }
    q = static_cast<u32>(qy) * c.width + static_cast<u32>(qx);
    return true;
}

// --- DI reuse -------------------------------------------------------------------------------------------------
/// restir.di.temporal (mode 0) / restir.di.spatial (mode 1, `iteration`).
template <typename Env>
inline RestirDiReservoir rsDiReuse(Env& env, const RestirFrameConstants& c, u32 px, u32 py, u32 mode, u32 iteration) {
    const u32 pixel = py * c.width + px;
    const RestirSurfaceF s = env.surface(0u, pixel);
    const RestirDiReservoir canon = env.diSource(pixel);
    if (!rsValid(s)) {
        return canon;
    }
    RestirDiReservoir in[1u + kRestirMaxNeighbors];
    RestirSurfaceF sf[1u + kRestirMaxNeighbors];
    u32 count = 1u;
    in[0] = canon;
    sf[0] = s;
    u32 rng = rsSeed(px, py, c.frameIndex, kRsStreamDiReuse + mode * 16u + iteration, c.seed);
    if (mode == kRestirModeTemporal) {
        u32 q = 0u;
        if (rsReproject(env, c, px, py, q)) {
            const RestirSurfaceF sp = env.surface(1u, q);
            if (rsSimilar(c, s, sp)) {
                RestirDiReservoir h = env.diHistory(q);
                const f32 cap = c.diMCap * canon.M;
                h.M = h.M < cap ? h.M : cap;
                in[count] = h;
                sf[count] = sp;
                ++count;
            }
        }
    } else {
        const u32 neighbors = c.diNeighbors < kRestirMaxNeighbors ? c.diNeighbors : kRestirMaxNeighbors;
        for (u32 k = 0; k < neighbors; ++k) {
            u32 q = 0u;
            if (!rsNeighbor(c, px, py, c.diRadius, rng, q)) {
                continue;
            }
            const RestirSurfaceF sq = env.surface(0u, q);
            if (!rsSimilar(c, s, sq)) {
                continue;
            }
            in[count] = env.diSource(q);
            sf[count] = sq;
            ++count;
        }
    }
    const bool unbiased = (c.flags & kRestirFlagUnbiased) != 0u;
    f32 mSum = 0.f;
    for (u32 i = 0; i < count; ++i) {
        mSum = mSum + in[i].M;
    }
    RestirDiReservoir out{};
    out.M = mSum;
    f32 wSum = 0.f;
    f32 selPhat = 0.f;
    for (u32 i = 0; i < count; ++i) {
        const RestirDiReservoir ri = in[i];
        if (!(ri.W > 0.f) || ri.light == kRestirInvalid) {
            continue;
        }
        const f32 pc = rsDiTarget(env, c, s, ri, unbiased);
        if (!(pc > 0.f)) {
            continue;
        }
        f32 w = 0.f;
        if (unbiased) {
            f32 num = 0.f;
            f32 den = 0.f;
            for (u32 j = 0; j < count; ++j) {
                const f32 pj = j == 0u ? pc : rsDiTarget(env, c, sf[j], ri, true);
                const f32 mj = in[j].M * pj;
                den = den + mj;
                if (j == i) {
                    num = mj;
                }
            }
            const f32 m = num / den;
            w = (m * pc) * ri.W;
        } else {
            w = (pc * ri.W) * ri.M;
        }
        if (!(w > 0.f)) {
            continue;
        }
        wSum = wSum + w;
        const f32 u = rsNext(rng);
        if (u * wSum < w) {
            out.light = ri.light;
            out.u1 = ri.u1;
            out.u2 = ri.u2;
            selPhat = pc;
        }
    }
    if (out.light != kRestirInvalid) {
        out.W = unbiased ? wSum / selPhat : wSum / (selPhat * mSum);
        out.targetPdf = selPhat;
    }
    return out;
}

// --- GI -------------------------------------------------------------------------------------------------------
/// A GI sample seen from one visible point.
struct RsGiEval {
    f32 cosV = 0.f;
    f32 phat = 0.f; ///< luminance(Lo) cos(theta_x) (solid angle at the visible point)
    f32 area = 0.f; ///< cos(phi) / |x - x_s|^2: solid angle -> area density factor at x_s
    RV3 origin{};   ///< reconnection shadow ray
    RV3 dir{};
    f32 tMax = 0.f;
};

inline bool rsGiEvaluate(const RestirFrameConstants& c, const RestirSurfaceF& s, const RestirGiReservoir& r, RsGiEval& e) {
    e = RsGiEval{};
    if (!rsValid(s)) {
        return false;
    }
    const RV3 xs = rsLoad(r.position);
    const RV3 ns = rsLoad(r.normal);
    const RV3 d = rsSub(xs, s.p);
    const f32 dist2 = rsDot(d, d);
    if (!(dist2 > 1e-12f)) {
        return false;
    }
    const f32 dist = std::sqrt(dist2);
    const RV3 w{d.x / dist, d.y / dist, d.z / dist};
    const f32 cosV = rsDot(s.n, w);
    if (!(cosV > 0.f)) {
        return false;
    }
    const f32 cosS = -rsDot(ns, w);
    if (!(cosS > 0.f)) {
        return false;
    }
    const f32 phat = rsLum(rsLoad(r.radiance)) * cosV;
    if (!(phat > 0.f)) {
        return false;
    }
    e.cosV = cosV;
    e.phat = phat;
    e.area = cosS / dist2;
    e.origin = rsOffset(c, s.p, s.n);
    const RV3 target = rsAdd(xs, rsScale(ns, c.normalBias));
    const RV3 to = rsSub(target, e.origin);
    const f32 dl = rsLength(to);
    if (dl > 1e-6f) {
        e.dir = RV3{to.x / dl, to.y / dl, to.z / dl};
        e.tMax = dl * kRestirShadowShorten;
    } else {
        e.dir = s.n;
        e.tMax = 0.f;
    }
    return true;
}

/// Cosine-weighted direction around unit n from (u, v) (concentric disk + Malley).
inline RV3 rsCosineDirection(const RV3& n, f32 u, f32 v) {
    f32 dx = 0.f;
    f32 dy = 0.f;
    light_tree::lt_concentric(u, v, dx, dy);
    const f32 xx = dx * dx;
    const f32 yy = dy * dy;
    const f32 r = 1.f - xx;
    const f32 dz = light_tree::lt_safe_sqrt(r - yy);
    RV3 t{};
    RV3 b{};
    rsBasis(n, t, b);
    const RV3 d = rsAdd(rsAdd(rsScale(t, dx), rsScale(b, dy)), rsScale(n, dz));
    return rsNormalize(d, n);
}

/// restir.gi.initial: one cosine-weighted secondary ray, next-event estimation at the hit. `record` (optional)
/// receives the traced ray's hit.
template <typename Env>
inline RestirGiReservoir rsGiInitial(Env& env, const RestirFrameConstants& c, u32 px, u32 py, RestirGiHitRecord* record) {
    const u32 pixel = py * c.width + px;
    RestirGiReservoir r{};
    if (record != nullptr) {
        *record = RestirGiHitRecord{};
    }
    const RestirSurfaceF s = env.surface(0u, pixel);
    if (!rsValid(s)) {
        return r;
    }
    r.M = 1.f;
    f32 u = 0.f;
    f32 v = 0.f;
    rsSobol(c.frameIndex, rsSeed(px, py, 0u, kRsStreamGiDir, c.seed), u, v);
    const RV3 dir = rsCosineDirection(s.n, u, v);
    const f32 cosV = rsDot(s.n, dir);
    if (record != nullptr) {
        rsStore(dir, record->direction);
    }
    if (!(cosV > 0.f)) {
        return r;
    }
    RestirHitF hit{};
    const bool found = env.traceHit(s.p, dir, c.giRayTMin, c.farDistance, hit);
    if (record != nullptr) {
        record->flags = kRestirHitTraced | (found ? kRestirHitHit : 0u);
        record->t = found ? hit.t : -1.f;
        record->instance = found ? hit.instance : kRestirInvalid;
        record->primitive = found ? hit.primitive : kRestirInvalid;
    }
    if (!found) {
        return r;
    }
    const RV3 xs = rsAdd(s.p, rsScale(dir, hit.t));
    RV3 ns = rsNormalize(hit.normal, rsNeg(dir));
    if (rsDot(ns, dir) > 0.f) {
        ns = rsNeg(ns);
    }
    RestirSurfaceF hs{};
    hs.p = xs;
    hs.depth = 1.f;
    hs.n = ns;
    u32 rng = rsSeed(px, py, c.frameIndex, kRsStreamGiNee, c.seed);
    const f32 u0 = rsNext(rng);
    f32 u1 = 0.f;
    f32 u2 = 0.f;
    rsSobol(c.frameIndex, rsSeed(px, py, 0u, kRsStreamGiNeeSobol, c.seed), u1, u2);
    const f32 hp[3] = {xs.x, xs.y, xs.z};
    const f32 hn[3] = {ns.x, ns.y, ns.z};
    const LightTreeSample ls = light_tree::lt_sample(env.tree(), hp, hn, u0, u1, u2);
    RV3 lo{};
    if (ls.light != light_tree::kLtInvalid) {
        const f32 src = rsSourcePdf(ls.kind, ls.pmf, ls.pdfArea);
        RsLightEval e{};
        if (src > 0.f && rsEvalLight(env, c, hs, ls.light, u1, u2, e)) {
            if (!(e.tMax > 0.f) || !env.occluded(e.origin, e.dir, e.tMax)) {
                lo.x = ((hit.albedo.x * e.contrib.x) / kRestirPi) / src;
                lo.y = ((hit.albedo.y * e.contrib.y) / kRestirPi) / src;
                lo.z = ((hit.albedo.z * e.contrib.z) / kRestirPi) / src;
            }
        }
    }
    rsStore(xs, r.position);
    rsStore(ns, r.normal);
    rsStore(lo, r.radiance);
    const f32 phat = rsLum(lo) * cosV;
    if (phat > 0.f) {
        r.W = kRestirPi / cosV;
        r.targetPdf = phat;
    }
    return r;
}

/// Area-measure target of GI sample r at surface s, visibility-tested (unbiased MIS weights).
template <typename Env>
inline f32 rsGiAreaTarget(Env& env, const RestirFrameConstants& c, const RestirSurfaceF& s, const RestirGiReservoir& r) {
    RsGiEval e{};
    if (!rsGiEvaluate(c, s, r, e)) {
        return 0.f;
    }
    if (e.tMax > 0.f && env.occluded(e.origin, e.dir, e.tMax)) {
        return 0.f;
    }
    return e.phat * e.area;
}

/// restir.gi.temporal (mode 0) / restir.gi.spatial (mode 1, `iteration`).
template <typename Env>
inline RestirGiReservoir rsGiReuse(Env& env, const RestirFrameConstants& c, u32 px, u32 py, u32 mode, u32 iteration) {
    const u32 pixel = py * c.width + px;
    const RestirSurfaceF s = env.surface(0u, pixel);
    const RestirGiReservoir canon = env.giSource(pixel);
    if (!rsValid(s)) {
        return canon;
    }
    RestirGiReservoir in[1u + kRestirMaxNeighbors];
    RestirSurfaceF sf[1u + kRestirMaxNeighbors];
    u32 count = 1u;
    in[0] = canon;
    sf[0] = s;
    u32 rng = rsSeed(px, py, c.frameIndex, kRsStreamGiReuse + mode * 16u + iteration, c.seed);
    if (mode == kRestirModeTemporal) {
        u32 q = 0u;
        if (rsReproject(env, c, px, py, q)) {
            const RestirSurfaceF sp = env.surface(1u, q);
            if (rsSimilar(c, s, sp)) {
                RestirGiReservoir h = env.giHistory(q);
                const f32 cap = c.giMCap * canon.M;
                h.M = h.M < cap ? h.M : cap;
                in[count] = h;
                sf[count] = sp;
                ++count;
            }
        }
    } else {
        const u32 neighbors = c.giNeighbors < kRestirMaxNeighbors ? c.giNeighbors : kRestirMaxNeighbors;
        for (u32 k = 0; k < neighbors; ++k) {
            u32 q = 0u;
            if (!rsNeighbor(c, px, py, c.giRadius, rng, q)) {
                continue;
            }
            const RestirSurfaceF sq = env.surface(0u, q);
            if (!rsSimilar(c, s, sq)) {
                continue;
            }
            in[count] = env.giSource(q);
            sf[count] = sq;
            ++count;
        }
    }
    const bool unbiased = (c.flags & kRestirFlagUnbiased) != 0u;
    f32 mSum = 0.f;
    for (u32 i = 0; i < count; ++i) {
        mSum = mSum + in[i].M;
    }
    RestirGiReservoir out{};
    out.M = mSum;
    f32 wSum = 0.f;
    f32 selPhat = 0.f;
    bool selected = false;
    for (u32 i = 0; i < count; ++i) {
        const RestirGiReservoir ri = in[i];
        if (!(ri.W > 0.f)) {
            continue;
        }
        RsGiEval ec{};
        if (!rsGiEvaluate(c, s, ri, ec)) {
            continue;
        }
        if (unbiased && ec.tMax > 0.f && env.occluded(ec.origin, ec.dir, ec.tMax)) {
            continue;
        }
        const f32 pc = ec.phat;
        f32 jacobian = 1.f;
        if (i != 0u) {
            RsGiEval ei{};
            if (!rsGiEvaluate(c, sf[i], ri, ei)) {
                continue;
            }
            jacobian = ec.area / ei.area;
            if (!unbiased && c.giJacobianClamp > 0.f &&
                (jacobian > c.giJacobianClamp || jacobian * c.giJacobianClamp < 1.f)) {
                continue;
            }
        }
        f32 w = 0.f;
        if (unbiased) {
            f32 num = 0.f;
            f32 den = 0.f;
            for (u32 j = 0; j < count; ++j) {
                const f32 pj = j == 0u ? pc * ec.area : rsGiAreaTarget(env, c, sf[j], ri);
                const f32 mj = in[j].M * pj;
                den = den + mj;
                if (j == i) {
                    num = mj;
                }
            }
            const f32 m = num / den;
            w = ((m * pc) * ri.W) * jacobian;
        } else {
            w = ((pc * ri.W) * jacobian) * ri.M;
        }
        if (!(w > 0.f)) {
            continue;
        }
        wSum = wSum + w;
        const f32 u = rsNext(rng);
        if (u * wSum < w) {
            for (u32 k = 0; k < 3u; ++k) {
                out.position[k] = ri.position[k];
                out.normal[k] = ri.normal[k];
                out.radiance[k] = ri.radiance[k];
            }
            selPhat = pc;
            selected = true;
        }
    }
    if (selected) {
        out.W = unbiased ? wSum / selPhat : wSum / (selPhat * mSum);
        out.targetPdf = selPhat;
    }
    return out;
}

// --- shade ----------------------------------------------------------------------------------------------------
/// restir.shade: the final reservoirs -> demodulated DI / GI radiance (f32x4, w = 0) and the linear depth.
/// `di` / `gi` may be null (chain disabled: zero signal).
template <typename Env>
inline void rsShade(Env& env, const RestirFrameConstants& c, u32 px, u32 py, const RestirDiReservoir* di,
                    const RestirGiReservoir* gi, f32 diOut[4], f32 giOut[4], f32& depth) {
    const u32 pixel = py * c.width + px;
    for (u32 k = 0; k < 4u; ++k) {
        diOut[k] = 0.f;
        giOut[k] = 0.f;
    }
    const RestirSurfaceF s = env.surface(0u, pixel);
    depth = s.depth;
    if (!rsValid(s)) {
        depth = 0.f;
        return;
    }
    if (di != nullptr && di->light != kRestirInvalid && di->W > 0.f) {
        RsLightEval e{};
        if (rsEvalLight(env, c, s, di->light, di->u1, di->u2, e)) {
            if (!(e.tMax > 0.f) || !env.occluded(e.origin, e.dir, e.tMax)) {
                const f32 k = di->W / kRestirPi;
                diOut[0] = e.contrib.x * k;
                diOut[1] = e.contrib.y * k;
                diOut[2] = e.contrib.z * k;
            }
        }
    }
    if (gi != nullptr && gi->W > 0.f) {
        RsGiEval e{};
        if (rsGiEvaluate(c, s, *gi, e)) {
            const bool trace = (c.flags & kRestirFlagGiShadeVisibility) != 0u && e.tMax > 0.f;
            if (!trace || !env.occluded(e.origin, e.dir, e.tMax)) {
                const f32 k = (e.cosV * gi->W) / kRestirPi;
                giOut[0] = gi->radiance[0] * k;
                giOut[1] = gi->radiance[1] * k;
                giOut[2] = gi->radiance[2] * k;
            }
        }
    }
}

} // namespace fuse::renderer::restir
