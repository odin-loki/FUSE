// B5.3 — PBR BRDF helpers (P5 §5.3 reference shader)
// CPU mirror: fuse::renderer::Brdf (src/material/brdf.cpp).
// WP-2.2: multi-scatter energy compensation (fuse_brdf_*), CPU twin namespace fuse::renderer::brdf in
// include/fuse/renderer/material/brdf.hpp (same expressions, same order); Slang twin brdf.slang.

#ifndef FUSE_BRDF_GLSL
#define FUSE_BRDF_GLSL

const float FUSE_PI = 3.14159265358979;
const float FUSE_MIN_ROUGHNESS = 0.045;

// GGX normal distribution function — D term
float D_GGX(float NoH, float roughness) {
    float r  = clamp(roughness, FUSE_MIN_ROUGHNESS, 1.0);
    float a  = r * r;
    float a2 = a * a;
    float d  = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (FUSE_PI * d * d);
}

// Height-correlated Smith visibility — V = G / (4 NoV NoL). It already contains the
// Cook-Torrance denominator, so the specular lobe is D * V * F (no further division).
float G_SmithGGX(float NoV, float NoL, float roughness) {
    float r  = clamp(roughness, FUSE_MIN_ROUGHNESS, 1.0);
    float a  = r * r;
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

// Schlick Fresnel — F term
vec3 F_Schlick(float VoH, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0);
}

// Full Cook-Torrance BRDF evaluation
vec3 brdf_evaluate(
    vec3  albedo,
    float roughness,
    float metallic,
    vec3  N,
    vec3  V,
    vec3  L,
    vec3  light_color,
    float light_intensity
) {
    vec3  H    = normalize(V + L);
    float NoV  = max(dot(N, V), 1e-4);
    float NoL  = max(dot(N, L), 0.0);
    float NoH  = max(dot(N, H), 0.0);
    float VoH  = max(dot(V, H), 0.0);

    metallic   = clamp(metallic, 0.0, 1.0);
    vec3  F0   = mix(vec3(0.04), albedo, metallic);
    vec3  F    = F_Schlick(VoH, F0);
    float D    = D_GGX(NoH, roughness);
    float Vis  = G_SmithGGX(NoV, NoL, roughness);

    vec3  spec = D * Vis * F;
    vec3  diff = (1.0 - F) * (1.0 - metallic) * albedo / FUSE_PI;

    return (diff + spec) * light_color * light_intensity * NoL;
}

// --- WP-2.2 multi-scatter energy compensation (Fdez-Aguera 2019; see brdf.hpp) -------------------
// A, B: the DFG split at (N.V, roughness) (lighting/ltc LUT): integral of f cos = F0 A + B.

const float FUSE_DIELECTRIC_F0 = 0.04;
const float FUSE_MIN_NOV = 1e-4;

// Left-to-right like fuse::math::Vec3::dot.
float fuse_brdf_dot(vec3 a, vec3 b) {
    precise float r = a.x * b.x + a.y * b.y + a.z * b.z;
    return r;
}

float fuse_brdf_fresnel_average(float f0) {
    precise float r = f0 + (1.0 - f0) * (1.0 / 21.0);
    return r;
}

// comp(F0) = 1 + F_avg E_ms / (1 - F_avg E_ms), E_ms = 1 - (A + B).
float fuse_brdf_energy_compensation(float f0, float dfgA, float dfgB) {
    precise float ems = 1.0 - (dfgA + dfgB);
    precise float k = fuse_brdf_fresnel_average(f0) * ems;
    precise float r = 1.0 + k / max(1.0 - k, 1e-4);
    return r;
}

struct FuseMsTerms {
    float metallic;       // saturated
    float compDielectric; // comp(0.04)
    vec3 compMetal;       // comp(albedo)
    vec3 diffuse;         // (1 - m) (1 - E_spec(0.04)) albedo
    vec3 specular;        // (1 - m) E_spec(0.04) + m E_spec(albedo)
};

FuseMsTerms fuse_brdf_ms_terms(vec3 albedo, float metallic, float dfgA, float dfgB) {
    FuseMsTerms t;
    const float m = clamp(metallic, 0.0, 1.0);
    t.metallic = m;
    t.compDielectric = fuse_brdf_energy_compensation(FUSE_DIELECTRIC_F0, dfgA, dfgB);
    precise float eD = (FUSE_DIELECTRIC_F0 * dfgA + dfgB) * t.compDielectric;
    precise float kd = (1.0 - m) * (1.0 - eD);
    for (int c = 0; c < 3; ++c) {
        const float a = albedo[c];
        const float comp = fuse_brdf_energy_compensation(a, dfgA, dfgB);
        t.compMetal[c] = comp;
        precise float diffuse = kd * a;
        precise float specular = (1.0 - m) * eD + m * ((a * dfgA + dfgB) * comp);
        t.diffuse[c] = diffuse;
        t.specular[c] = specular;
    }
    return t;
}

// GGX D x height-correlated Smith V (roughness clamped; nDotV already floored); oneMinusNoH2 =
// |N x H|^2 (no cancellation near the peak, see brdf.hpp).
float fuse_brdf_ggx_dv(float nDotH, float oneMinusNoH2, float nDotV, float nDotL, float roughness) {
    precise float r = clamp(roughness, FUSE_MIN_ROUGHNESS, 1.0);
    precise float a = r * r;
    precise float a2 = a * a;
    precise float dd = oneMinusNoH2 + nDotH * nDotH * a2;
    precise float d = a2 / (float(FUSE_PI) * dd * dd);
    precise float gv = nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2);
    precise float gl = nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2);
    precise float vis = 0.5 / max(gv + gl, 1e-5);
    precise float dv = d * vis;
    return dv;
}

// Compensated BRDF x N.L (0 when N.L <= 0).
vec3 fuse_brdf_ms_cos(FuseMsTerms t, vec3 albedo, float roughness, vec3 n, vec3 v, vec3 l) {
    const float nDotL = fuse_brdf_dot(n, l);
    if (!(nDotL > 0.0)) {
        return vec3(0.0);
    }
    precise vec3 hv = v + l;
    precise float len2 = fuse_brdf_dot(hv, hv);
    precise float invLen = 1.0 / sqrt(len2);
    precise vec3 hs = hv * invLen;
    const vec3 h = len2 > 0.0 ? hs : n;
    const float nDotV = max(fuse_brdf_dot(n, v), FUSE_MIN_NOV);
    const float nDotH = max(fuse_brdf_dot(n, h), 0.0);
    const float vDotH = max(fuse_brdf_dot(v, h), 0.0);
    precise vec3 nxh = vec3(n.y * h.z - n.z * h.y, n.z * h.x - n.x * h.z, n.x * h.y - n.y * h.x);
    const float oneMinusNoH2 = nDotH > 0.0 ? min(fuse_brdf_dot(nxh, nxh), 1.0) : 1.0;
    const float dv = fuse_brdf_ggx_dv(nDotH, oneMinusNoH2, nDotV, nDotL, roughness);
    precise float f = clamp(1.0 - vDotH, 0.0, 1.0);
    precise float f2 = f * f;
    precise float f5 = f2 * f2 * f;
    precise float fd = (FUSE_DIELECTRIC_F0 + (1.0 - FUSE_DIELECTRIC_F0) * f5) * t.compDielectric * (1.0 - t.metallic);
    vec3 result;
    for (int c = 0; c < 3; ++c) {
        const float a = albedo[c];
        precise float fm = (a + (1.0 - a) * f5) * t.compMetal[c] * t.metallic;
        precise float r = (dv * (fd + fm) + t.diffuse[c] * (1.0 / float(FUSE_PI))) * nDotL;
        result[c] = r;
    }
    return result;
}

#endif // FUSE_BRDF_GLSL
