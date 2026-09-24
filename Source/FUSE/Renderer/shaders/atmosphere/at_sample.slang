// WP-8.2 Hillaire atmosphere: the AtParams record and the LUT sampling helpers. Slang twin of at_sample.glsl
// (see there for the consumer contract). Consumer use: #include "atmosphere/at_sample.slang",
// AtParams atm = at_load(address); at_sky_radiance(atm, dir, true), at_sun_illuminance_at(atm, pos), at_aerial(...).
#pragma once

static const uint AT_FLAG_MULTI_SCATTER = 1u << 0;
static const float AT_PI = 3.14159265358979323846;
static const float AT_SUN_LIMB_U = 0.6;

// AtParams, 320 bytes.
struct AtParams {
    uint64_t transmittance;
    uint64_t multiscatter;
    uint64_t skyView;
    uint64_t aerialScatter;
    uint64_t aerialTransmittance;
    uint64_t reserved0;
    uint transWidth;
    uint transHeight;
    uint msWidth;
    uint msHeight;
    uint skyWidth;
    uint skyHeight;
    uint apWidth;
    uint apHeight;
    uint apDepth;
    uint flags;
    uint transSteps;
    uint msSteps;
    uint msDirSqrt;
    uint skySteps;
    uint apStepsPerSlice;
    uint reserved1;
    float bottomRadius;
    float topRadius;
    float rayleighHeight;
    float mieHeight;
    float rayleighScattering[4];
    float mieScattering;
    float mieExtinction;
    float mieG;
    float msFactor;
    float ozoneAbsorption[4];
    float ozoneCenter;
    float ozoneHalfWidth;
    float sunAngularRadius;
    float sunDiskNorm;
    float groundAlbedo[4];
    float sunIlluminance[4];
    float sunDir[4];
    float up[4];
    float cameraPos[4];
    float camForward[4];
    float camRight[4];
    float camUp[4];
};

AtParams at_load(uint64_t address) { return ((AtParams*)address)[0]; }
float4* at_texels(uint64_t address) { return (float4*)address; }

float at_clamp(float v, float lo, float hi) { return max(lo, min(hi, v)); }

// --- geometry (cancellation-free in f32) ------------------------------------------------------------------
float at_height(AtParams P, float r0, float mu, float t) {
    const float rg = P.bottomRadius;
    const float h0 = r0 - rg;
    const float k = t * (2.0 * r0 * mu + t);
    const float numerator = h0 * (r0 + rg) + k;
    const float r = sqrt(max(0.0, r0 * r0 + k));
    return numerator / max(1.0, r + rg);
}

float at_distance_to_top(AtParams P, float r0, float mu) {
    const float rt = P.topRadius;
    const float inside = (rt - r0) * (rt + r0);
    const float disc = inside + r0 * r0 * mu * mu;
    if (disc < 0.0) {
        return 0.0;
    }
    const float root = sqrt(disc);
    const float t = mu > 0.0 ? inside / max(1e-6, r0 * mu + root) : -r0 * mu + root;
    return max(0.0, t);
}

float at_distance_to_ground(AtParams P, float r0, float mu) {
    if (mu >= 0.0) {
        return -1.0;
    }
    const float rg = P.bottomRadius;
    const float above = (r0 - rg) * (r0 + rg);
    const float disc = r0 * r0 * mu * mu - above;
    if (disc < 0.0) {
        return -1.0;
    }
    const float denom = -r0 * mu + sqrt(disc);
    if (denom <= 0.0) {
        return 0.0;
    }
    return max(0.0, above / denom);
}

// --- parametrisations -------------------------------------------------------------------------------------
void at_transmittance_uv(AtParams P, float r, float mu, out float xMu, out float xR) {
    const float rg = P.bottomRadius;
    const float rt = P.topRadius;
    const float H = sqrt((rt - rg) * (rt + rg));
    const float rc = at_clamp(r, rg, rt);
    const float rho = sqrt(max(0.0, (rc - rg) * (rc + rg)));
    const float d = at_distance_to_top(P, rc, mu);
    const float dMin = rt - rc;
    const float dMax = rho + H;
    xMu = dMax - dMin > 0.0 ? (d - dMin) / (dMax - dMin) : 0.0;
    xR = rho / H;
}

void at_sky_view_uv(AtParams P, float r, float mu, float lightViewCos, out float u, out float v) {
    const float rg = P.bottomRadius;
    const float vHorizon = sqrt(max(0.0, (r - rg) * (r + rg)));
    const float beta = acos(at_clamp(vHorizon / r, -1.0, 1.0));
    const float zenithHorizon = AT_PI - beta;
    const float viewZenith = acos(at_clamp(mu, -1.0, 1.0));
    if (viewZenith <= zenithHorizon) {
        const float c = viewZenith / zenithHorizon;
        v = 0.5 * (1.0 - sqrt(max(0.0, 1.0 - c)));
    } else {
        const float c = (viewZenith - zenithHorizon) / beta;
        v = 0.5 + 0.5 * sqrt(max(0.0, c));
    }
    u = sqrt(max(0.0, 0.5 - 0.5 * lightViewCos));
}

// --- filtering --------------------------------------------------------------------------------------------
/// Bilinear fetch of a w x h f32x4 table at texel coordinates (fx, fy), clamped (w, h >= 2).
float4 at_bilinear(uint64_t table, uint w, uint h, float fx, float fy) {
    const float cx = at_clamp(fx, 0.0, float(w - 1u));
    const float cy = at_clamp(fy, 0.0, float(h - 1u));
    const uint x0 = min(uint(cx), w - 2u);
    const uint y0 = min(uint(cy), h - 2u);
    const float tx = cx - float(x0);
    const float ty = cy - float(y0);
    float4* t = at_texels(table);
    const float4 a = t[y0 * w + x0];
    const float4 b = t[y0 * w + x0 + 1u];
    const float4 c = t[(y0 + 1u) * w + x0];
    const float4 d = t[(y0 + 1u) * w + x0 + 1u];
    const float4 top = a + (b - a) * tx;
    const float4 bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

/// Fraction of the sun disk above the horizon (centre `offset` radii above it): circular segment of height
/// h = 1 - |offset|, area (x - sin x) / (2 pi), x = 4 asin(sqrt(h / 2)); series for small x (no cancellation).
float at_visible_sun_fraction(float offset) {
    const float d = at_clamp(offset, -1.0, 1.0);
    const float h = 1.0 - abs(d);
    const float x = 4.0 * asin(sqrt(0.5 * h));
    float seg;
    if (x < 0.5) {
        const float x2 = x * x;
        seg = x * x2 * (1.0 / 6.0) * (1.0 - x2 * (1.0 / 20.0) * (1.0 - x2 * (1.0 / 42.0) * (1.0 - x2 * (1.0 / 72.0))));
    } else {
        seg = x - sin(x);
    }
    seg = seg / (2.0 * AT_PI);
    return d >= 0.0 ? 1.0 - seg : seg;
}

// --- LUT lookups --------------------------------------------------------------------------------------------
/// Transmittance from radius r along mu to the top of the atmosphere (0 when the planet occludes the ray).
float3 at_transmittance(AtParams P, float r, float mu) {
    if (P.transmittance == 0 || at_distance_to_ground(P, r, mu) >= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    float xMu;
    float xR;
    at_transmittance_uv(P, r, mu, xMu, xR);
    return at_bilinear(P.transmittance, P.transWidth, P.transHeight, xMu * float(P.transWidth - 1u),
                       xR * float(P.transHeight - 1u)).xyz;
}

/// Sun transmittance at radius r (sun cos zenith muS) x the visible fraction of the sun disk above the horizon.
float3 at_sun_transmittance(AtParams P, float r, float muS) {
    if (P.transmittance == 0) {
        return float3(0.0, 0.0, 0.0);
    }
    const float rg = P.bottomRadius;
    const float rc = max(r, rg);
    const float cosH = rg / rc;
    const float muH = -sqrt(max(0.0, (rc - rg) * (rc + rg))) / rc;
    const float cosS = sqrt(max(0.0, 1.0 - muS * muS));
    const float above = asin(at_clamp(muS * cosH - cosS * muH, -1.0, 1.0));
    const float fraction = at_visible_sun_fraction(above / P.sunAngularRadius);
    if (fraction <= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    const float muC = max(muS, muH + 1e-5 * cosH);
    float xMu;
    float xR;
    at_transmittance_uv(P, r, muC, xMu, xR);
    const float3 t = at_bilinear(P.transmittance, P.transWidth, P.transHeight, xMu * float(P.transWidth - 1u),
                               xR * float(P.transHeight - 1u)).xyz;
    return t * fraction;
}

/// Psi_ms (multi-scattered luminance transfer) at radius r for sun cos zenith muS.
float3 at_multiscatter(AtParams P, float r, float muS) {
    if (P.multiscatter == 0) {
        return float3(0.0, 0.0, 0.0);
    }
    const float xS = at_clamp(muS * 0.5 + 0.5, 0.0, 1.0);
    const float xR = at_clamp((r - P.bottomRadius) / (P.topRadius - P.bottomRadius), 0.0, 1.0);
    return at_bilinear(P.multiscatter, P.msWidth, P.msHeight, xS * float(P.msWidth - 1u), xR * float(P.msHeight - 1u)).xyz;
}

float3 at_up(AtParams P) { return float3(P.up[0], P.up[1], P.up[2]); }
float3 at_sun(AtParams P) { return float3(P.sunDir[0], P.sunDir[1], P.sunDir[2]); }
float at_dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float3 at_cross(float3 a, float3 b) { return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }

/// Sky-view LUT for a world direction from the camera (per unit irradiance, no sun disk).
float3 at_sky_view(AtParams P, float3 dirIn) {
    if (P.skyView == 0) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 dir = dirIn * (1.0 / sqrt(at_dot(dirIn, dirIn)));
    const float3 up = at_up(P);
    const float3 sun = at_sun(P);
    const float mu = at_dot(dir, up);
    float lvc = 1.0;
    float3 side = at_cross(up, dir);
    const float sideLen = sqrt(at_dot(side, side));
    if (sideLen > 1e-6) {
        side = side * (1.0 / sideLen);
        const float3 fwd = at_cross(side, up);
        const float lx = at_dot(sun, fwd);
        const float ly = at_dot(sun, side);
        const float l = sqrt(lx * lx + ly * ly);
        lvc = l > 1e-6 ? lx / l : 1.0;
    }
    float u;
    float v;
    at_sky_view_uv(P, P.up[3], mu, lvc, u, v);
    return at_bilinear(P.skyView, P.skyWidth, P.skyHeight, u * float(P.skyWidth - 1u), v * float(P.skyHeight - 1u)).xyz;
}

/// Aerial perspective at screen uv in [0, 1]^2 (y down) and view distance (metres): in-scattered radiance
/// per unit irradiance and rgb view transmittance.
void at_aerial(AtParams P, float2 uv, float distance, out float3 scatter, out float3 trans) {
    scatter = float3(0.0, 0.0, 0.0);
    trans = float3(1.0, 1.0, 1.0);
    if (P.aerialScatter == 0 || P.aerialTransmittance == 0) {
        return;
    }
    const uint w = P.apWidth;
    const uint h = P.apHeight;
    const uint d = P.apDepth;
    const float fx = uv.x * float(w) - 0.5;
    const float fy = uv.y * float(h) - 0.5;
    const float slice = sqrt(at_clamp(distance / P.cameraPos[3], 0.0, 1.0));
    const float fz = slice * float(d) - 1.0;
    const uint64_t stride = uint64_t(w) * uint64_t(h) * 16;
    if (fz < 0.0) {
        const float k = fz + 1.0;
        const float3 s0 = at_bilinear(P.aerialScatter, w, h, fx, fy).xyz;
        const float3 t0 = at_bilinear(P.aerialTransmittance, w, h, fx, fy).xyz;
        scatter = s0 * k;
        trans = float3(1.0, 1.0, 1.0) + (t0 - float3(1.0, 1.0, 1.0)) * k;
        return;
    }
    const float cz = min(fz, float(d - 1u));
    const uint z0 = min(uint(cz), d - 2u);
    const float tz = cz - float(z0);
    const float3 sA = at_bilinear(P.aerialScatter + uint64_t(z0) * stride, w, h, fx, fy).xyz;
    const float3 sB = at_bilinear(P.aerialScatter + uint64_t(z0 + 1u) * stride, w, h, fx, fy).xyz;
    const float3 tA = at_bilinear(P.aerialTransmittance + uint64_t(z0) * stride, w, h, fx, fy).xyz;
    const float3 tB = at_bilinear(P.aerialTransmittance + uint64_t(z0 + 1u) * stride, w, h, fx, fy).xyz;
    scatter = sA + (sB - sA) * tz;
    trans = tA + (tB - tA) * tz;
}

float3 at_illuminance(AtParams P) { return float3(P.sunIlluminance[0], P.sunIlluminance[1], P.sunIlluminance[2]); }

/// Sky radiance (x sunIlluminance) for a world direction, + the transmitted limb-darkened sun disk.
float3 at_sky_radiance(AtParams P, float3 dirIn, bool sunDisk) {
    const float3 dir = dirIn * (1.0 / sqrt(at_dot(dirIn, dirIn)));
    float3 L = at_sky_view(P, dir);
    if (sunDisk) {
        const float3 sun = at_sun(P);
        const float3 c = at_cross(dir, sun);
        const float sep = atan2(sqrt(at_dot(c, c)), at_dot(dir, sun));
        if (sep < P.sunAngularRadius) {
            const float rho = sep / P.sunAngularRadius;
            const float m = sqrt(max(0.0, 1.0 - rho * rho));
            const float disk = (1.0 - AT_SUN_LIMB_U * (1.0 - m)) * P.sunDiskNorm;
            const float mu = dir.x * P.up[0] + dir.y * P.up[1] + dir.z * P.up[2];
            L = L + at_transmittance(P, P.up[3], mu) * disk;
        }
    }
    return L * at_illuminance(P);
}

/// Sun illuminance reaching a world point (transmittance x planet-shadow penumbra x sunIlluminance).
float3 at_sun_illuminance_at(AtParams P, float3 worldPos) {
    const float3 q = float3(worldPos.x, worldPos.y + P.bottomRadius, worldPos.z);
    const float r = sqrt(at_dot(q, q));
    const float muS = (q.x * P.sunDir[0] + q.y * P.sunDir[1] + q.z * P.sunDir[2]) / r;
    return at_sun_transmittance(P, at_clamp(r, P.bottomRadius, P.topRadius), muS) * at_illuminance(P);
}

