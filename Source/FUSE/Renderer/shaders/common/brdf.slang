// WP-2.2 Slang twin of shaders/common/brdf.glsl's multi-scatter helpers (fuse_brdf_* there; CPU twin
// namespace fuse::renderer::brdf in include/fuse/renderer/material/brdf.hpp). Same expressions in
// the same order; compile with -fp-mode precise (no contracted multiply-adds).
#pragma once

static const float kFuseBrdfPi = 3.14159265358979;
static const float kFuseBrdfMinRoughness = 0.045;
static const float kFuseBrdfDielectricF0 = 0.04;
static const float kFuseBrdfMinNoV = 1e-4;

// Left-to-right like fuse::math::Vec3::dot.
float fuseBrdfDot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

float fuseBrdfFresnelAverage(float f0) { return f0 + (1.0 - f0) * (1.0 / 21.0); }

// comp(F0) = 1 + F_avg E_ms / (1 - F_avg E_ms), E_ms = 1 - (A + B).
float fuseBrdfEnergyCompensation(float f0, float dfgA, float dfgB) {
    const float ems = 1.0 - (dfgA + dfgB);
    const float k = fuseBrdfFresnelAverage(f0) * ems;
    return 1.0 + k / max(1.0 - k, 1e-4);
}

struct FuseMsTerms {
    float metallic;
    float compDielectric;
    float3 compMetal;
    float3 diffuse;
    float3 specular;
};

FuseMsTerms fuseBrdfMsTerms(float3 albedo, float metallic, float dfgA, float dfgB) {
    FuseMsTerms t;
    const float m = clamp(metallic, 0.0, 1.0);
    t.metallic = m;
    t.compDielectric = fuseBrdfEnergyCompensation(kFuseBrdfDielectricF0, dfgA, dfgB);
    const float eD = (kFuseBrdfDielectricF0 * dfgA + dfgB) * t.compDielectric;
    const float kd = (1.0 - m) * (1.0 - eD);
    for (int c = 0; c < 3; ++c) {
        const float a = albedo[c];
        const float comp = fuseBrdfEnergyCompensation(a, dfgA, dfgB);
        t.compMetal[c] = comp;
        t.diffuse[c] = kd * a;
        t.specular[c] = (1.0 - m) * eD + m * ((a * dfgA + dfgB) * comp);
    }
    return t;
}

// oneMinusNoH2 = |N x H|^2 (no cancellation near the peak, see brdf.hpp).
float fuseBrdfGgxDv(float nDotH, float oneMinusNoH2, float nDotV, float nDotL, float roughness) {
    const float r = clamp(roughness, kFuseBrdfMinRoughness, 1.0);
    const float a = r * r;
    const float a2 = a * a;
    const float dd = oneMinusNoH2 + nDotH * nDotH * a2;
    const float d = a2 / (kFuseBrdfPi * dd * dd);
    const float gv = nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2);
    const float gl = nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2);
    const float vis = 0.5 / max(gv + gl, 1e-5);
    return d * vis;
}

float3 fuseBrdfMsCos(FuseMsTerms t, float3 albedo, float roughness, float3 n, float3 v, float3 l) {
    const float nDotL = fuseBrdfDot(n, l);
    if (!(nDotL > 0.0)) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 hv = v + l;
    const float len2 = fuseBrdfDot(hv, hv);
    const float3 h = len2 > 0.0 ? hv * (1.0 / sqrt(len2)) : n;
    const float nDotV = max(fuseBrdfDot(n, v), kFuseBrdfMinNoV);
    const float nDotH = max(fuseBrdfDot(n, h), 0.0);
    const float vDotH = max(fuseBrdfDot(v, h), 0.0);
    const float3 nxh = float3(n.y * h.z - n.z * h.y, n.z * h.x - n.x * h.z, n.x * h.y - n.y * h.x);
    const float oneMinusNoH2 = nDotH > 0.0 ? min(fuseBrdfDot(nxh, nxh), 1.0) : 1.0;
    const float dv = fuseBrdfGgxDv(nDotH, oneMinusNoH2, nDotV, nDotL, roughness);
    const float f = clamp(1.0 - vDotH, 0.0, 1.0);
    const float f2 = f * f;
    const float f5 = f2 * f2 * f;
    const float fd = (kFuseBrdfDielectricF0 + (1.0 - kFuseBrdfDielectricF0) * f5) * t.compDielectric * (1.0 - t.metallic);
    float3 result;
    for (int c = 0; c < 3; ++c) {
        const float a = albedo[c];
        const float fm = (a + (1.0 - a) * f5) * t.compMetal[c] * t.metallic;
        result[c] = (dv * (fd + fm) + t.diffuse[c] * (1.0 / kFuseBrdfPi)) * nDotL;
    }
    return result;
}
