// FUSE WP-2.2: Slang twin of lc_ltc.glsl (see there; C++ reference ltc_kernel.hpp and the WP-2.2 half of
// lighting_gpu::light_contribution). #include after lc_common.slang; compile with -fp-mode precise.
#pragma once
#include "../common/brdf.slang"
#include "../shadow_vsm/vsm_shadow.slang" // WP-3.2 shadow lookup (fuseVsmShadow)

static const uint kFuseLightRect = 4u;
static const uint kFuseLightDisk = 5u;
static const float kFuseLtcMinSunCos = 0.999999;
static const uint kFuseLtcSize = 64u;
static const uint kFuseLtcOffset = 0u;
static const uint kFuseDfgOffset = 16384u;
static const uint kFuseSphereOffset = 24576u;
static const float kFuseLtcTwoPi = 6.28318530717959;

// --- LUT ---------------------------------------------------------------------------------------
void fuseLtcAxis(float x, out uint i0, out float w) {
    const float fx = clamp(x, 0.0, 1.0) * float(kFuseLtcSize - 1u);
    i0 = min(uint(fx), kFuseLtcSize - 2u);
    w = fx - float(i0);
}

float fuseLtcLerp2(float* lut, uint t00, uint comps, uint c, float wx, float wy) {
    const uint t10 = t00 + comps;
    const uint t01 = t00 + kFuseLtcSize * comps;
    const uint t11 = t01 + comps;
    const float top = lut[t00 + c] + (lut[t10 + c] - lut[t00 + c]) * wx;
    const float bottom = lut[t01 + c] + (lut[t11 + c] - lut[t01 + c]) * wx;
    return top + (bottom - top) * wy;
}

uint fuseLtcTexel(uint offset, uint comps, float x, float y, out float wx, out float wy) {
    uint ix;
    uint iy;
    fuseLtcAxis(x, ix, wx);
    fuseLtcAxis(y, iy, wy);
    return offset + (iy * kFuseLtcSize + ix) * comps;
}

float fuseLtcViewCoord(float nDotV) { return sqrt(clamp(1.0 - nDotV, 0.0, 1.0)); }
float fuseLtcRoughness(float roughness) { return clamp(roughness, kFuseBrdfMinRoughness, 1.0); }

float2 fuseLtcSampleDfg(float* lut, float nDotV, float roughness) {
    float wx;
    float wy;
    const uint t = fuseLtcTexel(kFuseDfgOffset, 2u, fuseLtcViewCoord(nDotV), fuseLtcRoughness(roughness), wx, wy);
    return float2(fuseLtcLerp2(lut, t, 2u, 0u, wx, wy), fuseLtcLerp2(lut, t, 2u, 1u, wx, wy));
}

float4 fuseLtcSampleMatrix(float* lut, float nDotV, float roughness) {
    float wx;
    float wy;
    const uint t = fuseLtcTexel(kFuseLtcOffset, 4u, fuseLtcViewCoord(nDotV), fuseLtcRoughness(roughness), wx, wy);
    return float4(fuseLtcLerp2(lut, t, 4u, 0u, wx, wy), fuseLtcLerp2(lut, t, 4u, 1u, wx, wy),
                  fuseLtcLerp2(lut, t, 4u, 2u, wx, wy), fuseLtcLerp2(lut, t, 4u, 3u, wx, wy));
}

float fuseLtcSampleSphere(float* lut, float z, float ff) {
    float wx;
    float wy;
    const uint t = fuseLtcTexel(kFuseSphereOffset, 1u, z * 0.5 + 0.5, ff, wx, wy);
    return fuseLtcLerp2(lut, t, 1u, 0u, wx, wy);
}

// --- LTC integration -----------------------------------------------------------------------------
float3 fuseLtcCross(float3 a, float3 b) {
    return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

float fuseLtcLength(float3 v) { return sqrt(fuseBrdfDot(v, v)); }

float3 fuseLtcSafeUnit(float3 v, float3 fallback) {
    const float len2 = fuseBrdfDot(v, v);
    if (!(len2 > 0.0) || isinf(len2) || isnan(len2)) {
        return fallback;
    }
    const float inv = 1.0 / sqrt(len2);
    return float3(v.x * inv, v.y * inv, v.z * inv);
}

struct FuseLtcFrame {
    float3 t1;
    float3 t2;
    float3 n;
    float4 m; // M^-1 = [[m.x, 0, m.y], [0, 1, 0], [m.z, 0, m.w]]
};

FuseLtcFrame fuseLtcMakeFrame(float3 n, float3 v) {
    FuseLtcFrame f;
    f.n = n;
    const float nDotV = fuseBrdfDot(n, v);
    const float3 tangent = v - n * nDotV;
    const float3 axis = abs(n.x) < 0.5 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    const float3 fallback = fuseLtcSafeUnit(axis - n * fuseBrdfDot(n, axis), float3(1.0, 0.0, 0.0));
    f.t1 = fuseLtcSafeUnit(tangent, fallback);
    f.t2 = fuseLtcCross(n, f.t1);
    f.m = float4(1.0, 0.0, 0.0, 1.0);
    return f;
}

float3 fuseLtcToLtc(FuseLtcFrame f, bool identity, float3 w) {
    const float x = fuseBrdfDot(w, f.t1);
    const float y = fuseBrdfDot(w, f.t2);
    const float z = fuseBrdfDot(w, f.n);
    if (identity) {
        return float3(x, y, z);
    }
    return float3(f.m.x * x + f.m.y * z, y, f.m.z * x + f.m.w * z);
}

float fuseLtcEdge(float3 a, float3 e) {
    const float3 b = a + e;
    const float la = fuseLtcLength(a);
    const float lb = fuseLtcLength(b);
    const float inv = 1.0 / max(la * lb, 1e-30);
    const float3 c = fuseLtcCross(a, e) * inv;
    const float s = fuseLtcLength(c);
    const float d = fuseBrdfDot(a, b) * inv;
    const float theta = atan2(s, d);
    const float k = s > 1e-7 ? theta / s : 1.0;
    return c.z * k;
}

float fuseLtcQuad(float3 a, float3 e0, float3 e1) {
    float3 p[4] = { a, a + e0, a + e0 + e1, a + e1 };
    float3 e[4] = { e0, e1, e0 * -1.0, e1 * -1.0 };
    bool above[4] = { p[0].z > 0.0, p[1].z > 0.0, p[2].z > 0.0, p[3].z > 0.0 };
    float sum = 0.0;
    if (above[0] && above[1] && above[2] && above[3]) {
        for (uint i = 0u; i < 4u; ++i) {
            sum += fuseLtcEdge(p[i], e[i]);
        }
    } else {
        float3 q[5];
        uint count = 0u;
        for (uint i = 0u; i < 4u; ++i) {
            const uint j = (i + 1u) & 3u;
            if (above[i]) {
                q[count] = p[i];
                count = count + 1u;
            }
            if (above[i] != above[j]) {
                const float t = p[i].z / (p[i].z - p[j].z);
                float3 x = p[i] + e[i] * t;
                x.z = 0.0;
                q[count] = x;
                count = count + 1u;
            }
        }
        if (count < 3u) {
            return 0.0;
        }
        for (uint i = 0u; i < count; ++i) {
            const uint j = i + 1u < count ? i + 1u : 0u;
            sum += fuseLtcEdge(q[i], q[j] - q[i]);
        }
    }
    return max(sum, 0.0) * (1.0 / kFuseLtcTwoPi);
}

float fuseLtcRect(FuseLtcFrame f, bool identity, float3 c, float3 ex, float3 ey) {
    if (!(fuseBrdfDot(fuseLtcCross(ex, ey), c) < 0.0)) {
        return 0.0;
    }
    const float3 lc = fuseLtcToLtc(f, identity, c);
    const float3 lx = fuseLtcToLtc(f, identity, ex);
    const float3 ly = fuseLtcToLtc(f, identity, ey);
    return fuseLtcQuad(lc - lx - ly, ly * 2.0, lx * 2.0);
}

static const uint kFuseLtcEllipseVertices = 32u;
static const float kFuseLtcEllipseBand = 0.25;
static const uint kFuseLtcConeSamples = 64u;

// ltc::ellipse_clipped_form_factor: area-preserving 32-gon of the ellipse, clipped on the fly.
float fuseLtcEllipseClipped(float3 lc, float3 v1, float3 v2) {
    const float scale = 1.0032221;
    const float stepC = 0.98078528;
    const float stepS = 0.19509032;
    const float3 a1 = v1 * scale;
    const float3 a2 = v2 * scale;
    float cs = 1.0;
    float sn = 0.0;
    const float3 first = lc + a1;
    float3 prev = first;
    float3 exitPoint = float3(0.0, 0.0, 0.0);
    float3 entryPoint = float3(0.0, 0.0, 0.0);
    bool haveExit = false;
    bool haveEntry = false;
    float sum = 0.0;
    for (uint k = 1u; k <= kFuseLtcEllipseVertices; ++k) {
        const float nc = cs * stepC + sn * stepS;
        const float ns = sn * stepC - cs * stepS;
        cs = nc;
        sn = ns;
        const float3 cur = k == kFuseLtcEllipseVertices ? first : lc + a1 * cs + a2 * sn;
        const bool prevAbove = prev.z > 0.0;
        const bool curAbove = cur.z > 0.0;
        if (prevAbove && curAbove) {
            sum += fuseLtcEdge(prev, cur - prev);
        } else if (prevAbove != curAbove) {
            const float t = prev.z / (prev.z - cur.z);
            float3 x = prev + (cur - prev) * t;
            x.z = 0.0;
            if (prevAbove) {
                sum += fuseLtcEdge(prev, x - prev);
                exitPoint = x;
                haveExit = true;
            } else {
                sum += fuseLtcEdge(x, cur - x);
                entryPoint = x;
                haveEntry = true;
            }
        }
        prev = cur;
    }
    if (haveExit && haveEntry) {
        sum += fuseLtcEdge(exitPoint, entryPoint - exitPoint);
    }
    return max(sum, 0.0) * (1.0 / kFuseLtcTwoPi);
}

// ltc::cone_form_factor: an ellipse entirely above the horizon, the vector form factor's z as a
// boundary integral (64-point trapezoid rule).
float fuseLtcCone(float3 lc, float3 v1, float3 v2) {
    const float stepC = 0.99518473;
    const float stepS = 0.09801714;
    const float k1 = lc.x * v1.y - lc.y * v1.x;
    const float k2 = lc.x * v2.y - lc.y * v2.x;
    const float k3 = v1.x * v2.y - v1.y * v2.x;
    const float lc2 = fuseBrdfDot(lc, lc);
    const float half_ = 0.5 * (fuseBrdfDot(v1, v1) + fuseBrdfDot(v2, v2));
    const float invP0 = 1.0 / (lc2 + half_);
    float cs = 1.0;
    float sn = 0.0;
    float sum = 0.0;
    for (uint k = 0u; k < kFuseLtcConeSamples; ++k) {
        const float3 q = v1 * cs + v2 * sn;
        const float lq = fuseBrdfDot(lc, q);
        const float qq = fuseBrdfDot(q, q);
        const float p2 = lc2 + 2.0 * lq + qq;
        const float d = -2.0 * lq - (qq - half_);
        sum += ((cs * k2 - sn * k1) * d * invP0 + k3) / max(p2, 1e-30);
        const float nc = cs * stepC - sn * stepS;
        const float ns = sn * stepC + cs * stepS;
        cs = nc;
        sn = ns;
    }
    return clamp(-sum * (1.0 / float(kFuseLtcConeSamples)), 0.0, 1.0);
}

// ltc::disk_form_factor.
float fuseLtcDisk(FuseLtcFrame f, bool identity, float3 c, float3 ex, float3 ey) {
    if (!(fuseBrdfDot(fuseLtcCross(ex, ey), c) < 0.0)) {
        return 0.0;
    }
    const float3 lc = fuseLtcToLtc(f, identity, c);
    const float3 e1 = fuseLtcToLtc(f, identity, ex);
    const float3 e2 = fuseLtcToLtc(f, identity, ey);
    const float zSpan = sqrt(e1.z * e1.z + e2.z * e2.z);
    const float low = lc.z - zSpan;
    if (!(lc.z + zSpan > 0.0)) {
        return 0.0;
    }
    const float band = kFuseLtcEllipseBand * zSpan;
    const float polygon = low < band ? fuseLtcEllipseClipped(lc, e1, e2) : 0.0;
    if (!(low > 0.0)) {
        return polygon;
    }
    const float cone = fuseLtcCone(lc, e1, e2);
    return low < band ? polygon + (cone - polygon) * (low / band) : cone;
}

// --- area lights ---------------------------------------------------------------------------------
float fuseLtcSnorm16(uint bits) {
    const uint w = bits & 0xFFFFu;
    const int v = w >= 0x8000u ? int(w) - 65536 : int(w);
    return max(float(v) * (1.0 / 32767.0), -1.0);
}

float3 fuseLtcDecodeTangent(uint bits) {
    const float ox = fuseLtcSnorm16(bits);
    const float oy = fuseLtcSnorm16(bits >> 16u);
    float3 n = float3(ox, oy, 1.0 - abs(ox) - abs(oy));
    if (n.z < 0.0) {
        const float x = (1.0 - abs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
        const float y = (1.0 - abs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
        n.x = x;
        n.y = y;
    }
    return fuseLtcSafeUnit(n, float3(1.0, 0.0, 0.0));
}

void fuseLtcAreaAxes(float3 normal, float3 tangent, float halfX, float halfY, out float3 ex, out float3 ey) {
    const float3 axis = abs(normal.x) < 0.5 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    const float3 fallback = fuseLtcSafeUnit(axis - normal * fuseBrdfDot(normal, axis), float3(1.0, 0.0, 0.0));
    const float3 t = fuseLtcSafeUnit(tangent - normal * fuseBrdfDot(normal, tangent), fallback);
    const float3 b = fuseLtcCross(normal, t);
    ex = t * halfX;
    ey = b * halfY;
}

float3 fuseLtcAreaResponse(FuseLtcFrame f, FuseMsTerms t, bool disk, float3 c, float3 ex, float3 ey) {
    const float ffSpec = disk ? fuseLtcDisk(f, false, c, ex, ey) : fuseLtcRect(f, false, c, ex, ey);
    const float ffDiff = disk ? fuseLtcDisk(f, true, c, ex, ey) : fuseLtcRect(f, true, c, ex, ey);
    return float3(t.specular.x * ffSpec + t.diffuse.x * ffDiff, t.specular.y * ffSpec + t.diffuse.y * ffDiff,
                  t.specular.z * ffSpec + t.diffuse.z * ffDiff);
}

// F-free specular lobe D V N.L towards l (ltc::lobe_value).
float fuseLtcLobeValue(float3 n, float3 v, float3 l, float roughness) {
    const float nDotL = fuseBrdfDot(n, l);
    if (!(nDotL > 0.0)) {
        return 0.0;
    }
    const float3 h = fuseLtcSafeUnit(v + l, n);
    const float nDotH = max(fuseBrdfDot(n, h), 0.0);
    const float3 nxh = fuseLtcCross(n, h);
    const float oneMinusNoH2 = nDotH > 0.0 ? min(fuseBrdfDot(nxh, nxh), 1.0) : 1.0;
    return fuseBrdfGgxDv(nDotH, oneMinusNoH2, max(fuseBrdfDot(n, v), kFuseBrdfMinNoV), nDotL, roughness) * nDotL;
}

// Clipped / unclipped form factor of a cap (ltc::cap_clipped).
float fuseLtcCapClipped(float* lut, float z, float sinR) {
    if (z >= sinR) {
        return z;
    }
    if (z <= -sinR) {
        return 0.0;
    }
    if (sinR <= 0.1) {
        const float h = min(max(z / sinR, -1.0), 1.0);
        const float q = sqrt(max(1.0 - h * h, 0.0));
        const float g = h * (0.5 * kFuseBrdfPi + h * q + asin(h)) + (2.0 / 3.0) * q * q * q;
        return sinR * g * (1.0 / kFuseBrdfPi);
    }
    return fuseLtcSampleSphere(lut, z, sinR * sinR);
}

// Sun disk (ltc::sun_response).
float3 fuseLtcSunResponse(float* lut, FuseLtcFrame f, FuseMsTerms t, float3 albedo, float roughness, float3 v, float3 l,
                          float cosRadius) {
    const float c = min(max(cosRadius, -1.0), 1.0);
    const float sin2 = max(1.0 - c * c, 1e-12);
    const float sinR = sqrt(sin2);
    const float radius = acos(c);
    const float radiance = 1.0 / (kFuseBrdfPi * sin2);
    const float omega = kFuseLtcTwoPi * (1.0 - c);
    const float r = fuseLtcRoughness(roughness);
    const float alpha = r * r;
    const float nDotV = fuseBrdfDot(f.n, v);
    const float3 refl = f.n * (2.0 * nDotV) - v;
    const float cosAngle = min(max(fuseBrdfDot(refl, l), -1.0), 1.0);
    const float angle = acos(cosAngle);
    const float3 toward = fuseLtcSafeUnit(refl - l * cosAngle, l);
    const float3 near = angle <= radius ? refl : l * c + toward * sinR;
    const float lobeC = fuseLtcLobeValue(f.n, v, l, roughness);
    const float lobeP = fuseLtcLobeValue(f.n, v, near, roughness);
    const float wVar = lobeP > 0.0 ? (lobeC > 0.0 ? clamp((lobeP / lobeC - 1.5) * (1.0 / 1.5), 0.0, 1.0) : 1.0) : 0.0;
    const float wCentre = clamp(2.0 * radius / alpha - 1.0, 0.0, 1.0) * clamp(1.0 - (angle - radius) / (4.0 * alpha), 0.0, 1.0);
    const float w = max(wVar, wCentre);
    float ff = 0.0;
    if (w > 0.0) {
        const float tanR = sinR / max(c, 1e-6);
        const float3 axis = abs(l.x) < 0.5 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
        const float3 u = fuseLtcSafeUnit(axis - l * fuseBrdfDot(l, axis), float3(1.0, 0.0, 0.0));
        ff = fuseLtcDisk(f, false, l, u * tanR, fuseLtcCross(u, l) * tanR);
    }
    FuseMsTerms specOnly = t;
    specOnly.diffuse = float3(0.0, 0.0, 0.0);
    const float3 exact = fuseBrdfMsCos(specOnly, albedo, roughness, f.n, v, l);
    const float kExact = radiance * omega * (1.0 - w);
    const float kLtc = radiance * ff * w;
    const float diff = fuseLtcCapClipped(lut, fuseBrdfDot(f.n, l), sinR) * (1.0 / kFuseBrdfPi);
    return float3(exact.x * kExact + t.specular.x * kLtc + t.diffuse.x * diff,
                  exact.y * kExact + t.specular.y * kLtc + t.diffuse.y * diff,
                  exact.z * kExact + t.specular.z * kLtc + t.diffuse.z * diff);
}

float fuseLtcAreaWindow(float distance, float range) {
    if (!(range > 0.0) || !(distance < range)) {
        return 0.0;
    }
    const float ratio = distance / range;
    const float ratio2 = ratio * ratio;
    const float w = clamp(1.0 - ratio2 * ratio2, 0.0, 1.0);
    return w * w;
}

// --- compensated per-light shading --------------------------------------------------------------
struct FuseLcTerms {
    FuseMsTerms ms;
    FuseLtcFrame frame;
};

FuseLcTerms fuseLcSurfaceTerms(LcSurface s, float3 v, float* lut) {
    FuseLcTerms t;
    const float nDotV = max(fuseBrdfDot(s.normal, v), kFuseBrdfMinNoV);
    const float2 dfg = fuseLtcSampleDfg(lut, nDotV, s.roughness);
    t.ms = fuseBrdfMsTerms(s.albedo, s.metallic, dfg.x, dfg.y);
    t.frame = fuseLtcMakeFrame(s.normal, v);
    t.frame.m = fuseLtcSampleMatrix(lut, nDotV, s.roughness);
    return t;
}

float3 fuseLcLightMs(GpuLight light, LcSurface s, float3 v, FuseLcTerms t, float* lut) {
    float3 response = float3(0.0, 0.0, 0.0);
    float attenuation = 1.0;
    if (light.type == kFuseLightDirectional) {
        const float3 l = fuseLcSafeNormalize(-fuseLcVec3(light.direction), float3(0.0, 0.0, 1.0));
        if (light.cosOuter < kFuseLtcMinSunCos) {
            response = fuseLtcSunResponse(lut, t.frame, t.ms, s.albedo, s.roughness, v, l, light.cosOuter);
        } else {
            response = fuseBrdfMsCos(t.ms, s.albedo, s.roughness, s.normal, v, l);
        }
    } else if (light.type == kFuseLightPoint || light.type == kFuseLightSpot) {
        const float3 toLight = fuseLcVec3(light.position) - s.position;
        const float distance = fuseLcLength(toLight);
        attenuation = fuseLcFalloff(distance, light.range);
        if (attenuation == 0.0) {
            return float3(0.0, 0.0, 0.0);
        }
        const float3 l = toLight * (1.0 / max(distance, 1e-6));
        if (light.type == kFuseLightSpot) {
            const float3 axis = fuseLcSafeNormalize(fuseLcVec3(light.direction), float3(0.0, 0.0, -1.0));
            attenuation = attenuation * fuseLcSpotCone(-fuseLcDot(l, axis), light.cosInner, light.cosOuter);
            if (attenuation == 0.0) {
                return float3(0.0, 0.0, 0.0);
            }
        }
        response = fuseBrdfMsCos(t.ms, s.albedo, s.roughness, s.normal, v, l);
    } else if (light.type == kFuseLightRect || light.type == kFuseLightDisk) {
        const float3 c = fuseLcVec3(light.position) - s.position;
        attenuation = fuseLtcAreaWindow(fuseLtcLength(c), light.range);
        if (attenuation == 0.0) {
            return float3(0.0, 0.0, 0.0);
        }
        const float3 normal = fuseLcSafeNormalize(fuseLcVec3(light.direction), float3(0.0, 0.0, -1.0));
        float3 ex;
        float3 ey;
        fuseLtcAreaAxes(normal, fuseLtcDecodeTangent(light.flags), light.cosInner, light.cosOuter, ex, ey);
        response = fuseLtcAreaResponse(t.frame, t.ms, light.type == kFuseLightDisk, c, ex, ey);
    } else {
        return float3(0.0, 0.0, 0.0);
    }
    const float scale = light.intensity * attenuation;
    return float3(response.x * light.color[0] * scale, response.y * light.color[1] * scale,
                  response.z * light.color[2] * scale);
}

// The shade's light loop, shared by light.shade (lc_shade.slang) and the forward transparency pass
// (fuseFwShade); twin of fuse_lc_shade_lights (lc_ltc.glsl). CPU reference: lighting_gpu::shade_pixel.
// WP-3.2: one light's contribution scaled by its shadow visibility (unchanged without shadows).
float3 fuseLcShadowed(uint64_t shadows, uint slot, LcSurface s, float3 c) {
    if (shadows == 0ull) {
        return c;
    }
    const float visibility = fuseVsmShadow(shadows, slot, s.position, s.normal);
    return float3(c.x * visibility, c.y * visibility, c.z * visibility);
}

// WP-6.2: ray-traced visibility of light `slot` at `pixel` from the RtfxShadowView at `rt` (twin of
// fuse_lc_rt_visibility, lc_ltc.glsl).
struct LcRtView { // RtfxShadowView (48 bytes)
    uint64_t visibility;
    uint width;
    uint height;
    uint count;
    uint pad0;
    uint slots[4];
    uint pad1[2];
};

bool fuseLcRtVisibility(uint64_t rt, uint slot, uint2 pixel, out float visibility) {
    visibility = 1.0;
    if (rt == 0ull) {
        return false;
    }
    const LcRtView view = ((LcRtView*)rt)[0];
    if (pixel.x >= view.width || pixel.y >= view.height) {
        return false;
    }
    for (uint c = 0u; c < min(view.count, 4u); ++c) {
        if (view.slots[c] == slot) {
            visibility = ((float*)view.visibility)[c * view.width * view.height + pixel.y * view.width + pixel.x];
            return true;
        }
    }
    return false;
}

float3 fuseLcShadowedPx(uint64_t shadows, uint64_t rt, uint2 pixel, uint slot, LcSurface s, float3 c) {
    float visibility;
    if (fuseLcRtVisibility(rt, slot, pixel, visibility)) {
        return float3(c.x * visibility, c.y * visibility, c.z * visibility);
    }
    return fuseLcShadowed(shadows, slot, s, c);
}

// The light loop with the pixel light.shade shades (WP-6.2 ray-traced visibility); `pixel` = ~0u: none.
float3 fuseLcShadeLightsPx(LcFrame F, LcSurface s, float3 v, uint cluster, uint2 pixel) {
    GpuLight* lights = fuseGpuSceneLights(fuseGpuScene(F.scene));
    const uint64_t shadows = uint64_t(F.shadowsLo) | (uint64_t(F.shadowsHi) << 32u);
    const uint64_t rt = pixel.x == 0xFFFFFFFFu ? 0ull : (uint64_t(F.rtShadowsLo) | (uint64_t(F.rtShadowsHi) << 32u));
    float3 radiance = s.emissive + fuseLcVec3(F.ambient) * s.albedo * s.ao;
    const bool compensated = F.brdfLut != 0ull;
    float* lut = (float*)F.brdfLut;
    FuseLcTerms terms = {};
    if (compensated) {
        terms = fuseLcSurfaceTerms(s, v, lut);
    }
    uint* directional = (uint*)F.directional;
    const uint directionalCount = ((uint*)F.listHeader)[kFuseLcHeaderDirectional];
    for (uint i = 0u; i < directionalCount; ++i) {
        const uint slot = directional[i];
        if (slot < F.lightCount) {
            radiance = radiance + fuseLcShadowedPx(shadows, rt, pixel, slot, s,
                                                   compensated ? fuseLcLightMs(lights[slot], s, v, terms, lut) : fuseLcLight(lights[slot], s, v));
        }
    }
    uint* grid = (uint*)F.grid;
    uint* list = (uint*)F.lightList;
    const uint offset = grid[cluster * 2u];
    const uint count = grid[cluster * 2u + 1u];
    const bool skipArea = pixel.x != 0xFFFFFFFFu && (F.flags & kFuseLcFlagSkipAreaLights) != 0u;
    for (uint i = 0u; i < count; ++i) {
        const uint slot = list[offset + i];
        if (slot < F.lightCount &&
            !(skipArea && (lights[slot].type == kFuseLightRect || lights[slot].type == kFuseLightDisk))) {
            radiance = radiance + fuseLcShadowedPx(shadows, rt, pixel, slot, s,
                                                   compensated ? fuseLcLightMs(lights[slot], s, v, terms, lut) : fuseLcLight(lights[slot], s, v));
        }
    }
    return radiance;
}

float3 fuseLcShadeLights(LcFrame F, LcSurface s, float3 v, uint cluster) {
    return fuseLcShadeLightsPx(F, s, v, cluster, uint2(0xFFFFFFFFu, 0xFFFFFFFFu));
}
