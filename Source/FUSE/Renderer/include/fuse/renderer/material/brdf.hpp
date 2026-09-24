#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

/// WP-2.2 device-safe BRDF helpers (single-source: the CPU references and the kernels' C++ twins use
/// them; shaders/common/brdf.{glsl,slang} mirror them expression for expression).
///
/// Multi-scatter energy compensation (Fdez-Aguera, "A Multiple-Scattering Microfacet Model for
/// Real-Time Image-based Lighting", JCGT 8(1), 2019; the energy argument of Kulla and Conty,
/// "Revisiting Physically Based Shading at Imageworks", SIGGRAPH 2017 course):
///   A, B     the single-scatter directional albedo split of the GGX lobe (the DFG LUT, baked by
///            ltc::DfgBakeKernel): integral of f cos = F0 A + B for Schlick F (A + B = E_ss, F = 1);
///   E_ms     = 1 - E_ss, F_avg = F0 + (1 - F0) / 21;
///   total    = E_ss(F0) + F_ms E_ms, F_ms = E_ss(F0) F_avg / (1 - E_ms F_avg), i.e. the single-scatter
///            specular lobe times comp(F0) = 1 + F_avg E_ms / (1 - F_avg E_ms)  (comp(1) = 1 / E_ss:
///            a white metal reflects everything at every roughness);
///   diffuse  the Lambert lobe weighted by 1 - E_spec(0.04) (the energy the dielectric's compensated
///            specular leaves), so an albedo-1 dielectric returns exactly 1 as well;
///   metallic an exact linear blend of the dielectric (F0 = 0.04) and metal (F0 = albedo) BRDFs
///            (Schlick is linear in F0, so the uncompensated specular equals the WP-2.1 F0 mix), which
///            keeps the white furnace at 1 for any metallic.
/// The per-light lobe is not reciprocal (the compensation and the diffuse weight depend on N.V only);
/// Kulla-Conty's reciprocal form needs E(N.L) per light.
namespace brdf {

inline constexpr f32 kPi = 3.14159265358979f;
inline constexpr f32 kDielectricF0 = 0.04f;
inline constexpr f32 kMinRoughness = 0.045f;
/// Floor of N.V, as in brdf_evaluate.
inline constexpr f32 kMinNoV = 1e-4f;

FUSE_HOST_DEVICE inline f32 saturate(f32 v) { return std::min(std::max(v, 0.f), 1.f); }

/// Cosine-weighted hemispherical average of Schlick's Fresnel: F0 + (1 - F0) / 21.
FUSE_HOST_DEVICE inline f32 fresnel_average(f32 f0) { return f0 + (1.f - f0) * (1.f / 21.f); }

/// Fdez-Aguera specular compensation factor comp(F0) for single-scatter albedo E_ss = A + B.
FUSE_HOST_DEVICE inline f32 energy_compensation(f32 f0, f32 dfg_a, f32 dfg_b) {
    const f32 ems = 1.f - (dfg_a + dfg_b);
    const f32 k = fresnel_average(f0) * ems;
    return 1.f + k / std::max(1.f - k, 1e-4f);
}

/// Directional albedo of the compensated specular lobe: (F0 A + B) comp(F0).
FUSE_HOST_DEVICE inline f32 specular_albedo(f32 f0, f32 dfg_a, f32 dfg_b) {
    return (f0 * dfg_a + dfg_b) * energy_compensation(f0, dfg_a, dfg_b);
}

/// The per-pixel terms of the compensated BRDF (they depend on N.V, roughness, albedo, metallic).
struct MultiScatterTerms {
    f32 metallic = 0.f;               ///< saturated
    f32 comp_dielectric = 1.f;        ///< comp(0.04)
    math::Vec3 comp_metal{1.f, 1.f, 1.f}; ///< comp(albedo) per channel
    math::Vec3 diffuse{};             ///< (1 - m) (1 - E_spec(0.04)) albedo: Lambert weight (x 1/pi)
    math::Vec3 specular{};            ///< (1 - m) E_spec(0.04) + m E_spec(albedo): specular albedo (LTC)
};

FUSE_HOST_DEVICE inline MultiScatterTerms multi_scatter_terms(const math::Vec3& albedo, f32 metallic, f32 dfg_a,
                                                             f32 dfg_b) {
    MultiScatterTerms t{};
    const f32 m = saturate(metallic);
    t.metallic = m;
    t.comp_dielectric = energy_compensation(kDielectricF0, dfg_a, dfg_b);
    const f32 e_d = (kDielectricF0 * dfg_a + dfg_b) * t.comp_dielectric;
    const f32 kd = (1.f - m) * (1.f - e_d);
    auto channel = [&](f32 a, f32& comp, f32& diffuse, f32& specular) {
        comp = energy_compensation(a, dfg_a, dfg_b);
        diffuse = kd * a;
        specular = (1.f - m) * e_d + m * ((a * dfg_a + dfg_b) * comp);
    };
    channel(albedo.x, t.comp_metal.x, t.diffuse.x, t.specular.x);
    channel(albedo.y, t.comp_metal.y, t.diffuse.y, t.specular.y);
    channel(albedo.z, t.comp_metal.z, t.diffuse.z, t.specular.z);
    return t;
}

/// GGX D * height-correlated Smith V (roughness clamped to kMinRoughness; n_dot_v already floored).
/// `one_minus_noh2` = 1 - N.H^2 computed as |N x H|^2: D's denominator 1 - N.H^2 (1 - a2) then has no
/// cancellation near the peak (the B5.3 form (N.H a2 - N.H) N.H + 1 loses ~ulp / a2 in f32, 1-3% at
/// the minimum roughness's a2 = 4e-6).
FUSE_HOST_DEVICE inline f32 ggx_dv(f32 n_dot_h, f32 one_minus_noh2, f32 n_dot_v, f32 n_dot_l, f32 roughness) {
    const f32 r = std::clamp(roughness, kMinRoughness, 1.f);
    const f32 a = r * r;
    const f32 a2 = a * a;
    const f32 dd = one_minus_noh2 + n_dot_h * n_dot_h * a2;
    const f32 d = a2 / (kPi * dd * dd);
    const f32 gv = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.f - a2) + a2);
    const f32 gl = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.f - a2) + a2);
    const f32 vis = 0.5f / std::max(gv + gl, 1e-5f);
    return d * vis;
}

/// Compensated BRDF x N.L for unit N, V, L (0 when N.L <= 0): per channel
///   D V N.L [(1 - m) F(0.04) comp(0.04) + m F(albedo) comp(albedo)] + diffuse / pi N.L.
FUSE_HOST_DEVICE inline math::Vec3 multi_scatter_cos(const MultiScatterTerms& t, const math::Vec3& albedo, f32 roughness,
                                                    const math::Vec3& n, const math::Vec3& v, const math::Vec3& l) {
    const f32 n_dot_l = n.dot(l);
    if (!(n_dot_l > 0.f)) {
        return {};
    }
    const math::Vec3 hv = v + l;
    const f32 len2 = hv.dot(hv);
    const math::Vec3 h = len2 > 0.f ? hv * (1.f / std::sqrt(len2)) : n;
    const f32 n_dot_v = std::max(n.dot(v), kMinNoV);
    const f32 n_dot_h = std::max(n.dot(h), 0.f);
    const f32 v_dot_h = std::max(v.dot(h), 0.f);
    const math::Vec3 nxh{n.y * h.z - n.z * h.y, n.z * h.x - n.x * h.z, n.x * h.y - n.y * h.x};
    const f32 one_minus_noh2 = n_dot_h > 0.f ? std::min(nxh.dot(nxh), 1.f) : 1.f;
    const f32 dv = ggx_dv(n_dot_h, one_minus_noh2, n_dot_v, n_dot_l, roughness);
    const f32 f = saturate(1.f - v_dot_h);
    const f32 f2 = f * f;
    const f32 f5 = f2 * f2 * f;
    const f32 fd = (kDielectricF0 + (1.f - kDielectricF0) * f5) * t.comp_dielectric * (1.f - t.metallic);
    auto channel = [&](f32 a, f32 comp, f32 diffuse) {
        const f32 fm = (a + (1.f - a) * f5) * comp * t.metallic;
        return (dv * (fd + fm) + diffuse * (1.f / kPi)) * n_dot_l;
    };
    return {channel(albedo.x, t.comp_metal.x, t.diffuse.x), channel(albedo.y, t.comp_metal.y, t.diffuse.y),
            channel(albedo.z, t.comp_metal.z, t.diffuse.z)};
}

} // namespace brdf

/// CPU reference of shaders/common/brdf.glsl (B5.3): GGX D, height-correlated Smith visibility,
/// Schlick Fresnel, Lambert diffuse, metallic workflow with F0 = mix(0.04, albedo, metallic).
/// Roughness is perceptual; alpha = roughness^2.
struct Brdf {
    static constexpr f32 kPi = 3.14159265358979f;
    static constexpr f32 kDielectricF0 = 0.04f;
    /// Perceptual roughness floor — keeps D finite for mirror-like inputs.
    static constexpr f32 kMinRoughness = 0.045f;

    static f32 clampRoughness(f32 roughness);

    /// GGX / Trowbridge-Reitz normal distribution.
    static f32 dGgx(f32 NoH, f32 roughness);
    /// Height-correlated Smith visibility V = G2 / (4 NoV NoL) — already contains the
    /// Cook-Torrance denominator, so specular = D * V * F.
    static f32 visSmithGgxCorrelated(f32 NoV, f32 NoL, f32 roughness);
    static fuse::math::Vec3 fSchlick(f32 VoH, const fuse::math::Vec3& f0);
    static fuse::math::Vec3 specularF0(const fuse::math::Vec3& albedo, f32 metallic);

    /// BRDF value f(V, L) (no cosine, no light), in 1/sr.
    static fuse::math::Vec3 evaluate(const fuse::math::Vec3& albedo,
                                     f32 roughness,
                                     f32 metallic,
                                     const fuse::math::Vec3& N,
                                     const fuse::math::Vec3& V,
                                     const fuse::math::Vec3& L);

    /// Outgoing radiance for a unit-irradiance directional light: f(V, L) * NoL.
    /// Mirrors `brdf_evaluate` with light_color = 1 and light_intensity = 1.
    static fuse::math::Vec3 shade(const fuse::math::Vec3& albedo,
                                  f32 roughness,
                                  f32 metallic,
                                  const fuse::math::Vec3& N,
                                  const fuse::math::Vec3& V,
                                  const fuse::math::Vec3& L);

    // --- WP-2.2 multi-scatter energy compensation (see namespace brdf above) ------------------------

    /// Compensated BRDF f(V, L) (no cosine) given the DFG split (A, B) at N.V and roughness
    /// (ltc::sample_dfg). evaluate() stays the single-scatter WP-2.1 / B5.3 lobe.
    static fuse::math::Vec3 evaluateMultiScatter(const fuse::math::Vec3& albedo,
                                                 f32 roughness,
                                                 f32 metallic,
                                                 const fuse::math::Vec3& N,
                                                 const fuse::math::Vec3& V,
                                                 const fuse::math::Vec3& L,
                                                 f32 dfgA,
                                                 f32 dfgB);
    /// Fdez-Aguera compensation comp(F0) = 1 + F_avg E_ms / (1 - F_avg E_ms).
    static f32 energyCompensation(f32 f0, f32 dfgA, f32 dfgB);
};

} // namespace fuse::renderer
