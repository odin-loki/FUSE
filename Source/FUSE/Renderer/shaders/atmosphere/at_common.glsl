// WP-8.2 Hillaire atmosphere: the integrators and texel kernels of the LUT passes. GLSL twin of
// at_common.slang; CPU twin: src/atmosphere/atmosphere_luts.cpp (same operations, same order).
#ifndef FUSE_AT_COMMON_GLSL
#define FUSE_AT_COMMON_GLSL
#extension GL_GOOGLE_include_directive : require
#include "at_sample.glsl"

layout(push_constant) uniform AtPushBlock {
    uint64_t params;
    uint64_t src;
    uint64_t dst;
    uint mode;
    uint reserved;
} pc;

#define AT_INV_FOUR_PI (1.0 / (4.0 * AT_PI))
#define AT_MIN_ALTITUDE 1.0
#define AT_PHASE_SKY 0u
#define AT_PHASE_ISOTROPIC 1u

float at_one_minus_exp(float x) {
    if (x < 0.02) {
        return x * (1.0 - x * (0.5 - x * (1.0 / 6.0 - x * (1.0 / 24.0))));
    }
    return 1.0 - exp(-x);
}

float at_log_linear(float hA, float hB, float len, float scaleH) {
    const float dA = exp(-hA / scaleH);
    const float d = (hB - hA) / scaleH;
    if (abs(d) < 0.02) {
        return len * dA * (1.0 - d * (0.5 - d * (1.0 / 6.0 - d * (1.0 / 24.0))));
    }
    return len * dA * ((1.0 - exp(-d)) / d);
}

float at_ozone_density(AtParams P, float h) { return max(0.0, 1.0 - abs(h - P.ozoneCenter) / P.ozoneHalfWidth); }

float at_rayleigh_phase(float nu) { return (3.0 / (16.0 * AT_PI)) * (1.0 + nu * nu); }

float at_mie_phase(float nu, float g) {
    const float g2 = g * g;
    const float base = max(1e-6, 1.0 + g2 - 2.0 * g * nu);
    const float denom = base * sqrt(base);
    return (3.0 / (8.0 * AT_PI)) * ((1.0 - g2) * (1.0 + nu * nu)) / ((2.0 + g2) * denom);
}

struct AtMarch {
    vec3 L;
    vec3 fms;
    vec3 thr;
};

AtMarch at_march_init() {
    AtMarch m;
    m.L = vec3(0.0);
    m.fms = vec3(0.0);
    m.thr = vec3(1.0);
    return m;
}

void at_scatter_step(AtParams P, float r0, float mu, float nu, float muS0, float t, float dt, uint phaseMode, bool useMs,
                     float phaseR, float phaseM, inout AtMarch m) {
    const float h = at_height(P, r0, mu, t);
    const float r = P.bottomRadius + max(0.0, h);
    const float hc = max(0.0, h);
    const float dR = exp(-hc / P.rayleighHeight);
    const float dM = exp(-hc / P.mieHeight);
    const float dO = at_ozone_density(P, hc);
    const vec3 scatR = vec3(P.rayleighScattering[0] * dR, P.rayleighScattering[1] * dR, P.rayleighScattering[2] * dR);
    const float scatM = P.mieScattering * dM;
    const float extM = P.mieExtinction * dM;
    const vec3 ext = vec3(scatR.x + extM + P.ozoneAbsorption[0] * dO, scatR.y + extM + P.ozoneAbsorption[1] * dO,
                          scatR.z + extM + P.ozoneAbsorption[2] * dO);
    const float muS = at_clamp((r0 * muS0 + t * nu) / r, -1.0, 1.0);
    const vec3 sunT = at_sun_transmittance(P, r, muS);
    const vec3 scat = vec3(scatR.x + scatM, scatR.y + scatM, scatR.z + scatM);
    vec3 phaseScat;
    if (phaseMode == AT_PHASE_ISOTROPIC) {
        phaseScat = scat * AT_INV_FOUR_PI;
    } else {
        phaseScat = vec3(scatR.x * phaseR + scatM * phaseM, scatR.y * phaseR + scatM * phaseM,
                         scatR.z * phaseR + scatM * phaseM);
    }
    vec3 S = sunT * phaseScat;
    if (useMs) {
        S = S + at_multiscatter(P, r, muS) * scat;
    }
    for (uint c = 0u; c < 3u; ++c) {
        const float od = ext[c] * dt;
        const float w = ext[c] > 1e-20 ? at_one_minus_exp(od) / ext[c] : dt;
        m.L[c] = m.L[c] + m.thr[c] * (S[c] * w);
        m.fms[c] = m.fms[c] + m.thr[c] * (scat[c] * w);
        m.thr[c] = m.thr[c] * exp(-od);
    }
}

void at_march_quadratic(AtParams P, float r0, float mu, float nu, float muS0, float tEnd, uint steps, uint phaseMode,
                        bool useMs, inout AtMarch m) {
    const float phaseR = at_rayleigh_phase(nu);
    const float phaseM = at_mie_phase(nu, P.mieG);
    const float inv = 1.0 / float(steps);
    for (uint i = 0u; i < steps; ++i) {
        const float u0 = float(i) * inv;
        const float u1 = float(i + 1u) * inv;
        const float t0 = tEnd * u0 * u0;
        const float t1 = tEnd * u1 * u1;
        const float t = 0.5 * (t0 + t1);
        at_scatter_step(P, r0, mu, nu, muS0, t, t1 - t0, phaseMode, useMs, phaseR, phaseM, m);
    }
}

// --- texel kernels ------------------------------------------------------------------------------------------
vec4 at_transmittance_texel(AtParams P, uint x, uint y) {
    const float xMu = float(x) / float(P.transWidth - 1u);
    const float xR = float(y) / float(P.transHeight - 1u);
    const float rg = P.bottomRadius;
    const float rt = P.topRadius;
    const float H = sqrt((rt - rg) * (rt + rg));
    const float rho = H * xR;
    const float r = sqrt(rho * rho + rg * rg);
    const float dMin = rt - r;
    const float dMax = rho + H;
    const float d = dMin + xMu * (dMax - dMin);
    float mu = d <= 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = at_clamp(mu, -1.0, 1.0);
    const float len = at_distance_to_top(P, r, mu);
    const float dt = len / float(P.transSteps);
    float pathR = 0.0;
    float pathM = 0.0;
    float pathO = 0.0;
    float hPrev = at_height(P, r, mu, 0.0);
    float oPrev = at_ozone_density(P, hPrev);
    for (uint i = 1u; i <= P.transSteps; ++i) {
        const float hNext = at_height(P, r, mu, dt * float(i));
        const float oNext = at_ozone_density(P, hNext);
        pathR = pathR + at_log_linear(hPrev, hNext, dt, P.rayleighHeight);
        pathM = pathM + at_log_linear(hPrev, hNext, dt, P.mieHeight);
        pathO = pathO + 0.5 * (oPrev + oNext) * dt;
        hPrev = hNext;
        oPrev = oNext;
    }
    const float m = P.mieExtinction * pathM;
    const vec3 tau = vec3(P.rayleighScattering[0] * pathR + m + P.ozoneAbsorption[0] * pathO,
                          P.rayleighScattering[1] * pathR + m + P.ozoneAbsorption[1] * pathO,
                          P.rayleighScattering[2] * pathR + m + P.ozoneAbsorption[2] * pathO);
    return vec4(exp(-tau.x), exp(-tau.y), exp(-tau.z), 0.0);
}

vec4 at_multiscatter_texel(AtParams P, uint x, uint y) {
    const float muS = float(x) / float(P.msWidth - 1u) * 2.0 - 1.0;
    const float top = P.topRadius - P.bottomRadius;
    const float h = at_clamp(float(y) / float(P.msHeight - 1u) * top, AT_MIN_ALTITUDE, top - AT_MIN_ALTITUDE);
    const float r = P.bottomRadius + h;
    const float sunX = sqrt(max(0.0, 1.0 - muS * muS));
    const uint n = P.msDirSqrt;
    const float invN = 1.0 / float(n);
    vec3 sumL = vec3(0.0);
    vec3 sumF = vec3(0.0);
    for (uint a = 0u; a < n; ++a) {
        const float theta = 2.0 * AT_PI * ((float(a) + 0.5) * invN);
        const float ct = cos(theta);
        for (uint b = 0u; b < n; ++b) {
            const float cosPhi = 1.0 - 2.0 * ((float(b) + 0.5) * invN);
            const float sinPhi = sqrt(max(0.0, 1.0 - cosPhi * cosPhi));
            const float mu = cosPhi;
            const float nu = ct * sinPhi * sunX + cosPhi * muS;
            const float tGround = at_distance_to_ground(P, r, mu);
            const bool ground = tGround >= 0.0;
            const float tEnd = ground ? tGround : at_distance_to_top(P, r, mu);
            AtMarch m = at_march_init();
            at_march_quadratic(P, r, mu, nu, muS, tEnd, P.msSteps, AT_PHASE_ISOTROPIC, false, m);
            if (ground) {
                const float muG = at_clamp((r * muS + tEnd * nu) / P.bottomRadius, -1.0, 1.0);
                const vec3 sunT = at_sun_transmittance(P, P.bottomRadius, muG);
                const float k = max(0.0, muG) / AT_PI;
                m.L = m.L + vec3(m.thr.x * sunT.x * P.groundAlbedo[0] * k, m.thr.y * sunT.y * P.groundAlbedo[1] * k,
                                 m.thr.z * sunT.z * P.groundAlbedo[2] * k);
            }
            sumL = sumL + m.L;
            sumF = sumF + m.fms;
        }
    }
    const float invCount = invN * invN;
    const vec3 l2 = sumL * invCount;
    const vec3 f = sumF * invCount;
    const vec3 psi = vec3(l2.x / (1.0 - f.x), l2.y / (1.0 - f.y), l2.z / (1.0 - f.z));
    return vec4(psi * P.msFactor, max(f.x, max(f.y, f.z)));
}

vec3 at_integrate_sky_to(AtParams P, float r0, float mu, float nu, float muS0, float tEnd, uint steps) {
    AtMarch m = at_march_init();
    if (tEnd > 0.0) {
        at_march_quadratic(P, r0, mu, nu, muS0, tEnd, steps, AT_PHASE_SKY, (P.flags & AT_FLAG_MULTI_SCATTER) != 0u, m);
    }
    return m.L;
}

vec4 at_sky_view_texel(AtParams P, uint x, uint y) {
    const float u = float(x) / float(P.skyWidth - 1u);
    const float v = float(y) / float(P.skyHeight - 1u);
    const float r = P.up[3];
    const float muS0 = P.sunDir[3];
    const float rg = P.bottomRadius;
    const float vHorizon = sqrt(max(0.0, (r - rg) * (r + rg)));
    const float beta = acos(at_clamp(vHorizon / r, -1.0, 1.0));
    const float zenithHorizon = AT_PI - beta;
    float viewZenith;
    if (v < 0.5) {
        float c = 1.0 - 2.0 * v;
        c = 1.0 - c * c;
        viewZenith = zenithHorizon * c;
    } else {
        float c = 2.0 * v - 1.0;
        c = c * c;
        viewZenith = zenithHorizon + beta * c;
    }
    const float mu = cos(viewZenith);
    const float lvc = 1.0 - 2.0 * u * u;
    const float sinV = sqrt(max(0.0, 1.0 - mu * mu));
    const float sunX = sqrt(max(0.0, 1.0 - muS0 * muS0));
    const float nu = sinV * lvc * sunX + mu * muS0;
    // The row decides the ground test (the horizon row is a tangent ray: sky side).
    float tEnd;
    if (v > 0.5) {
        const float tGround = at_distance_to_ground(P, r, mu);
        tEnd = tGround >= 0.0 ? tGround : max(0.0, -r * mu);
    } else {
        tEnd = at_distance_to_top(P, r, mu);
    }
    return vec4(at_integrate_sky_to(P, r, mu, nu, muS0, tEnd, P.skySteps), 0.0);
}

#endif
