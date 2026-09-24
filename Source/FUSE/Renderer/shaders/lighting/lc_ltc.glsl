// FUSE WP-2.2: BRDF LUT sampling, LTC area lights, sun disks and the compensated per-light shading.
// GLSL twin of include/fuse/renderer/lighting/ltc/ltc_kernel.hpp and of the WP-2.2 half of
// lighting_gpu::light_contribution (clustered_gpu_kernel.hpp): same expressions in the same order,
// `precise` (no contracted multiply-adds). Slang twin: lc_ltc.slang. #include after lc_common.glsl.
#ifndef FUSE_LC_LTC_GLSL
#define FUSE_LC_LTC_GLSL

#include "../common/brdf.glsl"
#include "../shadow_vsm/vsm_shadow.glsl" // WP-3.2 shadow lookup (fuse_vsm_shadow)

#define FUSE_LIGHT_RECT 4u
#define FUSE_LIGHT_DISK 5u
#define FUSE_LTC_MIN_SUN_COS 0.999999
#define FUSE_LTC_SIZE 64u
#define FUSE_LTC_OFFSET 0u
#define FUSE_DFG_OFFSET 16384u
#define FUSE_SPHERE_OFFSET 24576u
#define FUSE_LTC_TWO_PI 6.28318530717959

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer FuseLcLutRef { float v[]; };

// --- LUT ---------------------------------------------------------------------------------------
void fuse_ltc_axis(float x, out uint i0, out float w) {
    precise float fx = clamp(x, 0.0, 1.0) * float(FUSE_LTC_SIZE - 1u);
    i0 = min(uint(fx), FUSE_LTC_SIZE - 2u);
    precise float r = fx - float(i0);
    w = r;
}

float fuse_ltc_lerp2(FuseLcLutRef lut, uint t00, uint comps, uint c, float wx, float wy) {
    const uint t10 = t00 + comps;
    const uint t01 = t00 + FUSE_LTC_SIZE * comps;
    const uint t11 = t01 + comps;
    precise float top = lut.v[t00 + c] + (lut.v[t10 + c] - lut.v[t00 + c]) * wx;
    precise float bottom = lut.v[t01 + c] + (lut.v[t11 + c] - lut.v[t01 + c]) * wx;
    precise float r = top + (bottom - top) * wy;
    return r;
}

uint fuse_ltc_texel(uint offset, uint comps, float x, float y, out float wx, out float wy) {
    uint ix;
    uint iy;
    fuse_ltc_axis(x, ix, wx);
    fuse_ltc_axis(y, iy, wy);
    return offset + (iy * FUSE_LTC_SIZE + ix) * comps;
}

float fuse_ltc_view_coord(float nDotV) { return sqrt(clamp(1.0 - nDotV, 0.0, 1.0)); }
float fuse_ltc_roughness(float roughness) { return clamp(roughness, FUSE_MIN_ROUGHNESS, 1.0); }

vec2 fuse_ltc_sample_dfg(FuseLcLutRef lut, float nDotV, float roughness) {
    float wx;
    float wy;
    const uint t = fuse_ltc_texel(FUSE_DFG_OFFSET, 2u, fuse_ltc_view_coord(nDotV), fuse_ltc_roughness(roughness), wx, wy);
    return vec2(fuse_ltc_lerp2(lut, t, 2u, 0u, wx, wy), fuse_ltc_lerp2(lut, t, 2u, 1u, wx, wy));
}

vec4 fuse_ltc_sample_matrix(FuseLcLutRef lut, float nDotV, float roughness) {
    float wx;
    float wy;
    const uint t = fuse_ltc_texel(FUSE_LTC_OFFSET, 4u, fuse_ltc_view_coord(nDotV), fuse_ltc_roughness(roughness), wx, wy);
    return vec4(fuse_ltc_lerp2(lut, t, 4u, 0u, wx, wy), fuse_ltc_lerp2(lut, t, 4u, 1u, wx, wy),
                fuse_ltc_lerp2(lut, t, 4u, 2u, wx, wy), fuse_ltc_lerp2(lut, t, 4u, 3u, wx, wy));
}

float fuse_ltc_sample_sphere(FuseLcLutRef lut, float z, float ff) {
    float wx;
    float wy;
    precise float x = z * 0.5 + 0.5;
    const uint t = fuse_ltc_texel(FUSE_SPHERE_OFFSET, 1u, x, ff, wx, wy);
    return fuse_ltc_lerp2(lut, t, 1u, 0u, wx, wy);
}

// --- LTC integration -----------------------------------------------------------------------------
vec3 fuse_ltc_cross(vec3 a, vec3 b) {
    precise vec3 r = vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    return r;
}

float fuse_ltc_length(vec3 v) { return sqrt(fuse_brdf_dot(v, v)); }

vec3 fuse_ltc_safe_unit(vec3 v, vec3 fallback) {
    precise float len2 = fuse_brdf_dot(v, v);
    if (!(len2 > 0.0) || isinf(len2) || isnan(len2)) {
        return fallback;
    }
    precise float inv = 1.0 / sqrt(len2);
    precise vec3 r = vec3(v.x * inv, v.y * inv, v.z * inv);
    return r;
}

struct FuseLtcFrame {
    vec3 t1;
    vec3 t2;
    vec3 n;
    vec4 m; // M^-1 = [[m.x, 0, m.y], [0, 1, 0], [m.z, 0, m.w]]
};

FuseLtcFrame fuse_ltc_make_frame(vec3 n, vec3 v) {
    FuseLtcFrame f;
    f.n = n;
    const float nDotV = fuse_brdf_dot(n, v);
    precise vec3 tangent = v - n * nDotV;
    const vec3 axis = abs(n.x) < 0.5 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    precise vec3 axisT = axis - n * fuse_brdf_dot(n, axis);
    const vec3 fallback = fuse_ltc_safe_unit(axisT, vec3(1.0, 0.0, 0.0));
    f.t1 = fuse_ltc_safe_unit(tangent, fallback);
    f.t2 = fuse_ltc_cross(n, f.t1);
    f.m = vec4(1.0, 0.0, 0.0, 1.0);
    return f;
}

vec3 fuse_ltc_to_ltc(FuseLtcFrame f, bool identity, vec3 w) {
    const float x = fuse_brdf_dot(w, f.t1);
    const float y = fuse_brdf_dot(w, f.t2);
    const float z = fuse_brdf_dot(w, f.n);
    if (identity) {
        return vec3(x, y, z);
    }
    precise vec3 r = vec3(f.m.x * x + f.m.y * z, y, f.m.z * x + f.m.w * z);
    return r;
}

float fuse_ltc_edge(vec3 a, vec3 e) {
    precise vec3 b = a + e;
    const float la = fuse_ltc_length(a);
    const float lb = fuse_ltc_length(b);
    precise float inv = 1.0 / max(la * lb, 1e-30);
    precise vec3 c = fuse_ltc_cross(a, e) * inv;
    const float s = fuse_ltc_length(c);
    precise float d = fuse_brdf_dot(a, b) * inv;
    const float theta = atan(s, d);
    precise float k = s > 1e-7 ? theta / s : 1.0;
    precise float r = c.z * k;
    return r;
}

float fuse_ltc_quad(vec3 a, vec3 e0, vec3 e1) {
    precise vec3 p1 = a + e0;
    precise vec3 p2 = a + e0 + e1;
    precise vec3 p3 = a + e1;
    const vec3 p[4] = vec3[4](a, p1, p2, p3);
    const vec3 e[4] = vec3[4](e0, e1, e0 * -1.0, e1 * -1.0);
    const bool above[4] = bool[4](p[0].z > 0.0, p[1].z > 0.0, p[2].z > 0.0, p[3].z > 0.0);
    precise float sum = 0.0;
    if (above[0] && above[1] && above[2] && above[3]) {
        for (uint i = 0u; i < 4u; ++i) {
            sum += fuse_ltc_edge(p[i], e[i]);
        }
    } else {
        vec3 q[5];
        uint count = 0u;
        for (uint i = 0u; i < 4u; ++i) {
            const uint j = (i + 1u) & 3u;
            if (above[i]) {
                q[count++] = p[i];
            }
            if (above[i] != above[j]) {
                precise float t = p[i].z / (p[i].z - p[j].z);
                precise vec3 x = p[i] + e[i] * t;
                x.z = 0.0;
                q[count++] = x;
            }
        }
        if (count < 3u) {
            return 0.0;
        }
        for (uint i = 0u; i < count; ++i) {
            const uint j = i + 1u < count ? i + 1u : 0u;
            precise vec3 edge = q[j] - q[i];
            sum += fuse_ltc_edge(q[i], edge);
        }
    }
    precise float r = max(sum, 0.0) * (1.0 / float(FUSE_LTC_TWO_PI));
    return r;
}

float fuse_ltc_rect(FuseLtcFrame f, bool identity, vec3 c, vec3 ex, vec3 ey) {
    if (!(fuse_brdf_dot(fuse_ltc_cross(ex, ey), c) < 0.0)) {
        return 0.0;
    }
    const vec3 lc = fuse_ltc_to_ltc(f, identity, c);
    const vec3 lx = fuse_ltc_to_ltc(f, identity, ex);
    const vec3 ly = fuse_ltc_to_ltc(f, identity, ey);
    precise vec3 corner = lc - lx - ly;
    precise vec3 x2 = lx * 2.0;
    precise vec3 y2 = ly * 2.0;
    return fuse_ltc_quad(corner, y2, x2);
}

#define FUSE_LTC_ELLIPSE_VERTICES 32u
#define FUSE_LTC_ELLIPSE_BAND 0.25
#define FUSE_LTC_CONE_SAMPLES 64u

// ltc::ellipse_clipped_form_factor: area-preserving 32-gon of the ellipse, clipped on the fly.
float fuse_ltc_ellipse_clipped(vec3 lc, vec3 v1, vec3 v2) {
    const float scale = 1.0032221;
    const float stepC = 0.98078528;
    const float stepS = 0.19509032;
    precise vec3 a1 = v1 * scale;
    precise vec3 a2 = v2 * scale;
    precise float cs = 1.0;
    precise float sn = 0.0;
    precise vec3 first = lc + a1;
    vec3 prev = first;
    vec3 exitPoint = vec3(0.0);
    vec3 entryPoint = vec3(0.0);
    bool haveExit = false;
    bool haveEntry = false;
    precise float sum = 0.0;
    for (uint k = 1u; k <= FUSE_LTC_ELLIPSE_VERTICES; ++k) {
        precise float nc = cs * stepC + sn * stepS;
        precise float ns = sn * stepC - cs * stepS;
        cs = nc;
        sn = ns;
        precise vec3 ring = lc + a1 * cs + a2 * sn;
        const vec3 cur = k == FUSE_LTC_ELLIPSE_VERTICES ? first : ring;
        const bool prevAbove = prev.z > 0.0;
        const bool curAbove = cur.z > 0.0;
        if (prevAbove && curAbove) {
            precise vec3 e = cur - prev;
            sum += fuse_ltc_edge(prev, e);
        } else if (prevAbove != curAbove) {
            precise float t = prev.z / (prev.z - cur.z);
            precise vec3 x = prev + (cur - prev) * t;
            x.z = 0.0;
            if (prevAbove) {
                precise vec3 e = x - prev;
                sum += fuse_ltc_edge(prev, e);
                exitPoint = x;
                haveExit = true;
            } else {
                precise vec3 e = cur - x;
                sum += fuse_ltc_edge(x, e);
                entryPoint = x;
                haveEntry = true;
            }
        }
        prev = cur;
    }
    if (haveExit && haveEntry) {
        precise vec3 chord = entryPoint - exitPoint;
        sum += fuse_ltc_edge(exitPoint, chord);
    }
    precise float r = max(sum, 0.0) * (1.0 / float(FUSE_LTC_TWO_PI));
    return r;
}

// ltc::cone_form_factor: an ellipse entirely above the horizon, the vector form factor's z as a
// boundary integral (64-point trapezoid rule).
float fuse_ltc_cone(vec3 lc, vec3 v1, vec3 v2) {
    const float stepC = 0.99518473;
    const float stepS = 0.09801714;
    precise float k1 = lc.x * v1.y - lc.y * v1.x;
    precise float k2 = lc.x * v2.y - lc.y * v2.x;
    precise float k3 = v1.x * v2.y - v1.y * v2.x;
    const float lc2 = fuse_brdf_dot(lc, lc);
    precise float half_ = 0.5 * (fuse_brdf_dot(v1, v1) + fuse_brdf_dot(v2, v2));
    precise float invP0 = 1.0 / (lc2 + half_);
    precise float cs = 1.0;
    precise float sn = 0.0;
    precise float sum = 0.0;
    for (uint k = 0u; k < FUSE_LTC_CONE_SAMPLES; ++k) {
        precise vec3 q = v1 * cs + v2 * sn;
        const float lq = fuse_brdf_dot(lc, q);
        const float qq = fuse_brdf_dot(q, q);
        precise float p2 = lc2 + 2.0 * lq + qq;
        precise float d = -2.0 * lq - (qq - half_);
        precise float term = ((cs * k2 - sn * k1) * d * invP0 + k3) / max(p2, 1e-30);
        sum += term;
        precise float nc = cs * stepC - sn * stepS;
        precise float ns = sn * stepC + cs * stepS;
        cs = nc;
        sn = ns;
    }
    precise float r = -sum * (1.0 / float(FUSE_LTC_CONE_SAMPLES));
    return clamp(r, 0.0, 1.0);
}

// ltc::disk_form_factor.
float fuse_ltc_disk(FuseLtcFrame f, bool identity, vec3 c, vec3 ex, vec3 ey) {
    if (!(fuse_brdf_dot(fuse_ltc_cross(ex, ey), c) < 0.0)) {
        return 0.0;
    }
    const vec3 lc = fuse_ltc_to_ltc(f, identity, c);
    const vec3 e1 = fuse_ltc_to_ltc(f, identity, ex);
    const vec3 e2 = fuse_ltc_to_ltc(f, identity, ey);
    precise float zSpan = sqrt(e1.z * e1.z + e2.z * e2.z);
    precise float low = lc.z - zSpan;
    precise float top = lc.z + zSpan;
    if (!(top > 0.0)) {
        return 0.0;
    }
    precise float band = FUSE_LTC_ELLIPSE_BAND * zSpan;
    const float polygon = low < band ? fuse_ltc_ellipse_clipped(lc, e1, e2) : 0.0;
    if (!(low > 0.0)) {
        return polygon;
    }
    const float cone = fuse_ltc_cone(lc, e1, e2);
    precise float blended = polygon + (cone - polygon) * (low / band);
    return low < band ? blended : cone;
}

// --- area lights ---------------------------------------------------------------------------------
float fuse_ltc_snorm16(uint bits) {
    const uint w = bits & 0xFFFFu;
    const int v = w >= 0x8000u ? int(w) - 65536 : int(w);
    precise float r = max(float(v) * (1.0 / 32767.0), -1.0);
    return r;
}

vec3 fuse_ltc_decode_tangent(uint bits) {
    const float ox = fuse_ltc_snorm16(bits);
    const float oy = fuse_ltc_snorm16(bits >> 16u);
    precise vec3 n = vec3(ox, oy, 1.0 - abs(ox) - abs(oy));
    if (n.z < 0.0) {
        precise float x = (1.0 - abs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
        precise float y = (1.0 - abs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
        n.x = x;
        n.y = y;
    }
    return fuse_ltc_safe_unit(n, vec3(1.0, 0.0, 0.0));
}

void fuse_ltc_area_axes(vec3 normal, vec3 tangent, float halfX, float halfY, out vec3 ex, out vec3 ey) {
    const vec3 axis = abs(normal.x) < 0.5 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    precise vec3 axisT = axis - normal * fuse_brdf_dot(normal, axis);
    const vec3 fallback = fuse_ltc_safe_unit(axisT, vec3(1.0, 0.0, 0.0));
    precise vec3 tt = tangent - normal * fuse_brdf_dot(normal, tangent);
    const vec3 t = fuse_ltc_safe_unit(tt, fallback);
    const vec3 b = fuse_ltc_cross(normal, t);
    precise vec3 x = t * halfX;
    precise vec3 y = b * halfY;
    ex = x;
    ey = y;
}

vec3 fuse_ltc_area_response(FuseLtcFrame f, FuseMsTerms t, bool disk, vec3 c, vec3 ex, vec3 ey) {
    const float ffSpec = disk ? fuse_ltc_disk(f, false, c, ex, ey) : fuse_ltc_rect(f, false, c, ex, ey);
    const float ffDiff = disk ? fuse_ltc_disk(f, true, c, ex, ey) : fuse_ltc_rect(f, true, c, ex, ey);
    precise vec3 r = vec3(t.specular.x * ffSpec + t.diffuse.x * ffDiff, t.specular.y * ffSpec + t.diffuse.y * ffDiff,
                          t.specular.z * ffSpec + t.diffuse.z * ffDiff);
    return r;
}

// F-free specular lobe D V N.L towards l (ltc::lobe_value).
float fuse_ltc_lobe_value(vec3 n, vec3 v, vec3 l, float roughness) {
    const float nDotL = fuse_brdf_dot(n, l);
    if (!(nDotL > 0.0)) {
        return 0.0;
    }
    precise vec3 hv = v + l;
    const vec3 h = fuse_ltc_safe_unit(hv, n);
    const float nDotH = max(fuse_brdf_dot(n, h), 0.0);
    const vec3 nxh = fuse_ltc_cross(n, h);
    const float oneMinusNoH2 = nDotH > 0.0 ? min(fuse_brdf_dot(nxh, nxh), 1.0) : 1.0;
    precise float r = fuse_brdf_ggx_dv(nDotH, oneMinusNoH2, max(fuse_brdf_dot(n, v), FUSE_MIN_NOV), nDotL, roughness) * nDotL;
    return r;
}

// Clipped / unclipped form factor of a cap (ltc::cap_clipped).
float fuse_ltc_cap_clipped(FuseLcLutRef lut, float z, float sinR) {
    if (z >= sinR) {
        return z;
    }
    if (z <= -sinR) {
        return 0.0;
    }
    if (sinR <= 0.1) {
        precise float h = min(max(z / sinR, -1.0), 1.0);
        precise float q = sqrt(max(1.0 - h * h, 0.0));
        precise float g = h * (0.5 * float(FUSE_PI) + h * q + asin(h)) + (2.0 / 3.0) * q * q * q;
        precise float r = sinR * g * (1.0 / float(FUSE_PI));
        return r;
    }
    precise float ff = sinR * sinR;
    return fuse_ltc_sample_sphere(lut, z, ff);
}

// Sun disk (ltc::sun_response).
vec3 fuse_ltc_sun_response(FuseLcLutRef lut, FuseLtcFrame f, FuseMsTerms t, vec3 albedo, float roughness, vec3 v, vec3 l,
                           float cosRadius) {
    const float c = min(max(cosRadius, -1.0), 1.0);
    precise float sin2 = max(1.0 - c * c, 1e-12);
    const float sinR = sqrt(sin2);
    const float radius = acos(c);
    precise float radiance = 1.0 / (float(FUSE_PI) * sin2);
    precise float omega = float(FUSE_LTC_TWO_PI) * (1.0 - c);
    const float r = fuse_ltc_roughness(roughness);
    precise float alpha = r * r;
    const float nDotV = fuse_brdf_dot(f.n, v);
    precise vec3 refl = f.n * (2.0 * nDotV) - v;
    const float cosAngle = min(max(fuse_brdf_dot(refl, l), -1.0), 1.0);
    const float angle = acos(cosAngle);
    precise vec3 towardRaw = refl - l * cosAngle;
    const vec3 toward = fuse_ltc_safe_unit(towardRaw, l);
    precise vec3 rim = l * c + toward * sinR;
    const vec3 near = angle <= radius ? refl : rim;
    const float lobeC = fuse_ltc_lobe_value(f.n, v, l, roughness);
    const float lobeP = fuse_ltc_lobe_value(f.n, v, near, roughness);
    precise float wVarRaw = (lobeP / lobeC - 1.5) * (1.0 / 1.5);
    const float wVar = lobeP > 0.0 ? (lobeC > 0.0 ? clamp(wVarRaw, 0.0, 1.0) : 1.0) : 0.0;
    precise float sharp = 2.0 * radius / alpha - 1.0;
    precise float nearPeak = 1.0 - (angle - radius) / (4.0 * alpha);
    precise float wCentre = clamp(sharp, 0.0, 1.0) * clamp(nearPeak, 0.0, 1.0);
    const float w = max(wVar, wCentre);
    float ff = 0.0;
    if (w > 0.0) {
        precise float tanR = sinR / max(c, 1e-6);
        const vec3 axis = abs(l.x) < 0.5 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
        precise vec3 axisT = axis - l * fuse_brdf_dot(l, axis);
        const vec3 u = fuse_ltc_safe_unit(axisT, vec3(1.0, 0.0, 0.0));
        precise vec3 ex = u * tanR;
        precise vec3 ey = fuse_ltc_cross(u, l) * tanR;
        ff = fuse_ltc_disk(f, false, l, ex, ey);
    }
    FuseMsTerms specOnly = t;
    specOnly.diffuse = vec3(0.0);
    const vec3 exact = fuse_brdf_ms_cos(specOnly, albedo, roughness, f.n, v, l);
    precise float kExact = radiance * omega * (1.0 - w);
    precise float kLtc = radiance * ff * w;
    precise float diff = fuse_ltc_cap_clipped(lut, fuse_brdf_dot(f.n, l), sinR) * (1.0 / float(FUSE_PI));
    precise vec3 result = vec3(exact.x * kExact + t.specular.x * kLtc + t.diffuse.x * diff,
                               exact.y * kExact + t.specular.y * kLtc + t.diffuse.y * diff,
                               exact.z * kExact + t.specular.z * kLtc + t.diffuse.z * diff);
    return result;
}

float fuse_ltc_area_window(float distance, float range) {
    if (!(range > 0.0) || !(distance < range)) {
        return 0.0;
    }
    precise float ratio = distance / range;
    precise float ratio2 = ratio * ratio;
    precise float w = clamp(1.0 - ratio2 * ratio2, 0.0, 1.0);
    precise float r = w * w;
    return r;
}

// --- compensated per-light shading (lighting_gpu::surface_terms / light_contribution with a LUT) ---
struct FuseLcTerms {
    FuseMsTerms ms;
    FuseLtcFrame frame;
};

FuseLcTerms fuse_lc_surface_terms(FuseLcSurface s, vec3 v, FuseLcLutRef lut) {
    FuseLcTerms t;
    const float nDotV = max(fuse_brdf_dot(s.normal, v), FUSE_MIN_NOV);
    const vec2 dfg = fuse_ltc_sample_dfg(lut, nDotV, s.roughness);
    t.ms = fuse_brdf_ms_terms(s.albedo, s.metallic, dfg.x, dfg.y);
    t.frame = fuse_ltc_make_frame(s.normal, v);
    t.frame.m = fuse_ltc_sample_matrix(lut, nDotV, s.roughness);
    return t;
}

vec3 fuse_lc_light_ms(FuseGpuLight light, FuseLcSurface s, vec3 v, FuseLcTerms t, FuseLcLutRef lut) {
    vec3 response = vec3(0.0);
    float attenuation = 1.0;
    if (light.type == FUSE_LIGHT_DIRECTIONAL) {
        const vec3 l = fuse_lc_safe_normalize(-fuse_lc_vec3(light.direction), vec3(0.0, 0.0, 1.0));
        if (light.cosOuter < FUSE_LTC_MIN_SUN_COS) {
            response = fuse_ltc_sun_response(lut, t.frame, t.ms, s.albedo, s.roughness, v, l, light.cosOuter);
        } else {
            response = fuse_brdf_ms_cos(t.ms, s.albedo, s.roughness, s.normal, v, l);
        }
    } else if (light.type == FUSE_LIGHT_POINT || light.type == FUSE_LIGHT_SPOT) {
        precise vec3 toLight = fuse_lc_vec3(light.position) - s.position;
        const float distance = fuse_lc_length(toLight);
        attenuation = fuse_lc_falloff(distance, light.range);
        if (attenuation == 0.0) {
            return vec3(0.0);
        }
        precise float inv = 1.0 / max(distance, 1e-6);
        precise vec3 l = toLight * inv;
        if (light.type == FUSE_LIGHT_SPOT) {
            const vec3 axis = fuse_lc_safe_normalize(fuse_lc_vec3(light.direction), vec3(0.0, 0.0, -1.0));
            precise float att = attenuation * fuse_lc_spot_cone(-fuse_lc_dot(l, axis), light.cosInner, light.cosOuter);
            attenuation = att;
            if (attenuation == 0.0) {
                return vec3(0.0);
            }
        }
        response = fuse_brdf_ms_cos(t.ms, s.albedo, s.roughness, s.normal, v, l);
    } else if (light.type == FUSE_LIGHT_RECT || light.type == FUSE_LIGHT_DISK) {
        precise vec3 c = fuse_lc_vec3(light.position) - s.position;
        attenuation = fuse_ltc_area_window(fuse_ltc_length(c), light.range);
        if (attenuation == 0.0) {
            return vec3(0.0);
        }
        const vec3 normal = fuse_lc_safe_normalize(fuse_lc_vec3(light.direction), vec3(0.0, 0.0, -1.0));
        vec3 ex;
        vec3 ey;
        fuse_ltc_area_axes(normal, fuse_ltc_decode_tangent(light.flags), light.cosInner, light.cosOuter, ex, ey);
        response = fuse_ltc_area_response(t.frame, t.ms, light.type == FUSE_LIGHT_DISK, c, ex, ey);
    } else {
        return vec3(0.0);
    }
    precise float scale = light.intensity * attenuation;
    precise vec3 r = vec3(response.x * light.color[0] * scale, response.y * light.color[1] * scale,
                          response.z * light.color[2] * scale);
    return r;
}

// The shade's light loop, shared by light.shade (lc_shade.comp) and the forward transparency pass
// (fuse_fw_shade): emissive + ambient x albedo x AO + the directional list + the cluster's lights
// (ascending slot); with the BRDF LUT (F.brdfLut != 0) the compensated fuse_lc_light_ms, else the
// WP-2.1 fuse_lc_light. CPU reference: lighting_gpu::shade_pixel (clustered_gpu_kernel.hpp).
// WP-3.2: one light's contribution scaled by its shadow visibility (unchanged without shadows).
vec3 fuse_lc_shadowed(uint64_t shadows, uint slot, FuseLcSurface s, vec3 c) {
    if (shadows == 0ul) {
        return c;
    }
    const float visibility = fuse_vsm_shadow(shadows, slot, s.position, s.normal);
    precise vec3 r = vec3(c.x * visibility, c.y * visibility, c.z * visibility);
    return r;
}

// WP-6.2: ray-traced visibility of light `slot` at `pixel` from the RtfxShadowView at `rt`
// (include/fuse/renderer/rt_effects/rt_effects_types.hpp). False when the light is not RT-shadowed or the
// pixel lies outside the view: the light then keeps its VSM visibility (or none).
struct FuseLcRtView { // RtfxShadowView (48 bytes)
    uint64_t visibility;
    uint width;
    uint height;
    uint count;
    uint pad0;
    uint slots[4];
    uint pad1[2];
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseLcRtViewRef { FuseLcRtView v; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer FuseLcRtFloatsRef { float v[]; };

bool fuse_lc_rt_visibility(uint64_t rt, uint slot, uvec2 pixel, out float visibility) {
    visibility = 1.0;
    if (rt == 0ul) {
        return false;
    }
    const FuseLcRtView view = FuseLcRtViewRef(rt).v;
    if (pixel.x >= view.width || pixel.y >= view.height) {
        return false;
    }
    for (uint c = 0u; c < min(view.count, 4u); ++c) {
        if (view.slots[c] == slot) {
            visibility = FuseLcRtFloatsRef(view.visibility).v[c * view.width * view.height + pixel.y * view.width + pixel.x];
            return true;
        }
    }
    return false;
}

vec3 fuse_lc_shadowed_px(uint64_t shadows, uint64_t rt, uvec2 pixel, uint slot, FuseLcSurface s, vec3 c) {
    float visibility;
    if (fuse_lc_rt_visibility(rt, slot, pixel, visibility)) {
        precise vec3 r = vec3(c.x * visibility, c.y * visibility, c.z * visibility);
        return r;
    }
    return fuse_lc_shadowed(shadows, slot, s, c);
}

// The light loop with the pixel light.shade shades (WP-6.2 ray-traced visibility); `pixel` = ~0u: none.
vec3 fuse_lc_shade_lights_px(FuseLcFrame F, FuseLcSurface s, vec3 v, uint cluster, uvec2 pixel) {
    FuseGpuLightsRef lights = fuse_gpu_scene_lights(fuse_gpu_scene(F.scene));
    const uint64_t shadows = uint64_t(F.shadowsLo) | (uint64_t(F.shadowsHi) << 32u);
    const uint64_t rt = pixel.x == 0xFFFFFFFFu ? 0ul : (uint64_t(F.rtShadowsLo) | (uint64_t(F.rtShadowsHi) << 32u));
    precise vec3 radiance = s.emissive + fuse_lc_vec3(F.ambient) * s.albedo * s.ao;
    const bool compensated = F.brdfLut != 0ul;
    FuseLcLutRef lut = FuseLcLutRef(F.brdfLut);
    FuseLcTerms terms;
    if (compensated) {
        terms = fuse_lc_surface_terms(s, v, lut);
    }
    FuseLcWordsRef directional = FuseLcWordsRef(F.directional);
    const uint directionalCount = FuseLcWordsRef(F.listHeader).v[FUSE_LC_HEADER_DIRECTIONAL];
    for (uint i = 0u; i < directionalCount; ++i) {
        const uint slot = directional.v[i];
        if (slot < F.lightCount) {
            radiance = radiance + fuse_lc_shadowed_px(shadows, rt, pixel, slot, s,
                                                      compensated ? fuse_lc_light_ms(lights.v[slot], s, v, terms, lut)
                                                                  : fuse_lc_light(lights.v[slot], s, v));
        }
    }
    FuseLcWordsRef grid = FuseLcWordsRef(F.grid);
    FuseLcWordsRef list = FuseLcWordsRef(F.lightList);
    const uint offset = grid.v[cluster * 2u];
    const uint count = grid.v[cluster * 2u + 1u];
    for (uint i = 0u; i < count; ++i) {
        const uint slot = list.v[offset + i];
        if (slot < F.lightCount) {
            radiance = radiance + fuse_lc_shadowed_px(shadows, rt, pixel, slot, s,
                                                      compensated ? fuse_lc_light_ms(lights.v[slot], s, v, terms, lut)
                                                                  : fuse_lc_light(lights.v[slot], s, v));
        }
    }
    return radiance;
}

vec3 fuse_lc_shade_lights(FuseLcFrame F, FuseLcSurface s, vec3 v, uint cluster) {
    return fuse_lc_shade_lights_px(F, s, v, cluster, uvec2(0xFFFFFFFFu));
}

#endif // FUSE_LC_LTC_GLSL
