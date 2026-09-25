// WP-8.3 volumetric clouds: Slang twin of cl_common.glsl (see there). Compile with -fp-mode precise and
// -I <Renderer>/shaders (the WP-8.2 at_sample.slang helpers).
#pragma once
#include "atmosphere/at_sample.slang"

static const uint CL_FLAG_HOMOGENEOUS = (1u << 0);
static const uint CL_FLAG_JITTER = (1u << 1);
static const uint CL_FLAG_HISTORY = (1u << 2);
static const uint CL_FLAG_NO_REPROJECT = (1u << 3);
static const uint CL_FLAG_ATMOSPHERE = (1u << 4);
static const uint CL_FLAG_AERIAL = (1u << 5);
static const uint CL_FLAG_BACKGROUND = (1u << 6);
static const uint CL_FLAG_SCENE_DEPTH = (1u << 7);
static const uint CL_FLAG_SUN_DISK = (1u << 8);
static const float CL_PI = 3.14159265358979323846;

// CloudParams, 496 bytes.
struct CloudParams {
    uint64_t shapeNoise;
    uint64_t detailNoise;
    uint64_t weather;
    uint64_t fresh;
    uint64_t historyIn;
    uint64_t historyOut;
    uint64_t result;
    uint64_t background;
    uint64_t depth;
    uint64_t atmosphere;
    uint shapeSize;
    uint detailSize;
    uint weatherSize;
    uint shapeFrequency;
    uint detailFrequency;
    uint weatherFrequency;
    uint seed;
    uint flags;
    uint width;
    uint height;
    uint freshWidth;
    uint freshHeight;
    uint block;
    uint offsetX;
    uint offsetY;
    uint frameIndex;
    uint outWidth;
    uint outHeight;
    uint primarySteps;
    uint lightSteps;
    uint octaves;
    uint cycle;
    uint reserved0;
    uint reserved1;
    float planetRadius;
    float cloudBottom;
    float cloudTop;
    float maxDistance;
    float shapeScale;
    float detailScale;
    float weatherScale;
    float coverageScale;
    float coverageBias;
    float densityScale;
    float detailStrength;
    float albedo;
    float phaseForward;
    float phaseBackward;
    float phaseBlend;
    float msAttenuation;
    float msExtinction;
    float msEccentricity;
    float powderStrength;
    float lightDistance;
    float ambientScale;
    float ambientBottom;
    float transmittanceCutoff;
    float staticThreshold;
    float maxHistoryCount;
    float motionHistoryCount;
    float depthRejection;
    float reserved3;
    float sunDir[4];
    float sunIlluminance[4];
    float ambient[4];
    float windOffset[4];
    float windDelta[4];
    float cameraPos[4];
    float camForward[4];
    float camRight[4];
    float camUp[4];
    float prevCameraPos[4];
    float prevForward[4];
    float prevRight[4];
    float prevUp[4];
};


struct CloudPush {
    uint64_t params;
    uint64_t src;
    uint64_t dst;
    uint mode;
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<CloudPush> pc;

CloudParams cl_load(uint64_t address) { return ((CloudParams*)address)[0]; }
float4* cl_texels(uint64_t address) { return (float4*)address; }
float* cl_floats(uint64_t address) { return (float*)address; }

float cl_saturate(float v) { return max(0.0, min(1.0, v)); }
float cl_lerp(float a, float b, float t) { return a + (b - a) * t; }
float cl_fract(float v) { return v - floor(v); }
float cl_dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float3 cl_v3(float a[4]) { return float3(a[0], a[1], a[2]); }

float cl_one_minus_exp(float x) {
    if (x < 0.02) {
        return x * (1.0 - x * (0.5 - x * (1.0 / 6.0 - x * (1.0 / 24.0))));
    }
    return 1.0 - exp(-x);
}

float cl_remap(float v, float lo, float hi, float nlo, float nhi) { return nlo + (v - lo) / max(1e-5, hi - lo) * (nhi - nlo); }

// --- noise ----------------------------------------------------------------------------------------------------
uint cl_hash(uint v) {
    const uint s = v * 747796405u + 2891336453u;
    const uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}

uint cl_hash4(uint x, uint y, uint z, uint seed) { return cl_hash(x + cl_hash(y + cl_hash(z + cl_hash(seed)))); }

float cl_unit(uint h) { return float(h >> 8u) * (1.0 / 16777216.0); }

float cl_grad(uint h, float x, float y, float z) {
    const uint h4 = h & 15u;
    const float u = h4 < 8u ? x : y;
    const float v = h4 < 4u ? y : ((h4 == 12u || h4 == 14u) ? x : z);
    return ((h4 & 1u) == 0u ? u : -u) + ((h4 & 2u) == 0u ? v : -v);
}

float cl_fade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

float cl_worley(float3 q, uint period, uint seed) {
    const float flx = floor(q.x);
    const float fly = floor(q.y);
    const float flz = floor(q.z);
    const float fx = q.x - flx;
    const float fy = q.y - fly;
    const float fz = q.z - flz;
    const uint ix = uint(flx);
    const uint iy = uint(fly);
    const uint iz = uint(flz);
    float best = 8.0;
    for (uint kz = 0u; kz < 3u; ++kz) {
        for (uint ky = 0u; ky < 3u; ++ky) {
            for (uint kx = 0u; kx < 3u; ++kx) {
                const uint cx = (ix + period - 1u + kx) % period;
                const uint cy = (iy + period - 1u + ky) % period;
                const uint cz = (iz + period - 1u + kz) % period;
                const uint h = cl_hash4(cx, cy, cz, seed);
                const float px = (float(kx) - 1.0) + cl_unit(h) - fx;
                const float py = (float(ky) - 1.0) + cl_unit(cl_hash(h)) - fy;
                const float pz = (float(kz) - 1.0) + cl_unit(cl_hash(h ^ 0x9e3779b9u)) - fz;
                const float d = px * px + py * py + pz * pz;
                best = min(best, d);
            }
        }
    }
    return 1.0 - min(1.0, sqrt(best));
}

float cl_perlin(float3 q, uint period, uint seed) {
    const float flx = floor(q.x);
    const float fly = floor(q.y);
    const float flz = floor(q.z);
    const float fx = q.x - flx;
    const float fy = q.y - fly;
    const float fz = q.z - flz;
    const uint x0 = uint(flx) % period;
    const uint y0 = uint(fly) % period;
    const uint z0 = uint(flz) % period;
    const uint x1 = (x0 + 1u) % period;
    const uint y1 = (y0 + 1u) % period;
    const uint z1 = (z0 + 1u) % period;
    const float n000 = cl_grad(cl_hash4(x0, y0, z0, seed), fx, fy, fz);
    const float n100 = cl_grad(cl_hash4(x1, y0, z0, seed), fx - 1.0, fy, fz);
    const float n010 = cl_grad(cl_hash4(x0, y1, z0, seed), fx, fy - 1.0, fz);
    const float n110 = cl_grad(cl_hash4(x1, y1, z0, seed), fx - 1.0, fy - 1.0, fz);
    const float n001 = cl_grad(cl_hash4(x0, y0, z1, seed), fx, fy, fz - 1.0);
    const float n101 = cl_grad(cl_hash4(x1, y0, z1, seed), fx - 1.0, fy, fz - 1.0);
    const float n011 = cl_grad(cl_hash4(x0, y1, z1, seed), fx, fy - 1.0, fz - 1.0);
    const float n111 = cl_grad(cl_hash4(x1, y1, z1, seed), fx - 1.0, fy - 1.0, fz - 1.0);
    const float u = cl_fade(fx);
    const float v = cl_fade(fy);
    const float w = cl_fade(fz);
    const float a = cl_lerp(cl_lerp(n000, n100, u), cl_lerp(n010, n110, u), v);
    const float b = cl_lerp(cl_lerp(n001, n101, u), cl_lerp(n011, n111, u), v);
    return cl_lerp(a, b, w);
}

float cl_perlin_fbm(float3 q, uint period, uint seed) {
    const float a = cl_perlin(q, period, seed);
    const float b = cl_perlin(q * 2.0, period * 2u, seed + 1u);
    const float c = cl_perlin(q * 4.0, period * 4u, seed + 2u);
    return (a + b * 0.5 + c * 0.25) * (1.0 / 1.75);
}

float cl_worley_fbm(float3 q, uint period, uint seed) {
    const float a = cl_worley(q, period, seed);
    const float b = cl_worley(q * 2.0, period * 2u, seed + 1u);
    const float c = cl_worley(q * 4.0, period * 4u, seed + 2u);
    return a * 0.625 + b * 0.25 + c * 0.125;
}

float3 cl_tile_coord(uint3 i, uint n) {
    const float inv = 1.0 / float(n);
    return float3((float(i.x) + 0.5) * inv, (float(i.y) + 0.5) * inv, (float(i.z) + 0.5) * inv);
}

float4 cl_shape_texel(CloudParams P, uint3 i) {
    const float3 t = cl_tile_coord(i, P.shapeSize);
    const uint F = P.shapeFrequency;
    const float ff = float(F);
    const float perlin = cl_saturate(cl_perlin_fbm(t * ff, F, P.seed) * 0.5 + 0.5);
    const float worley = cl_worley_fbm(t * ff, F, P.seed + 10u);
    float4 o;
    o.x = cl_remap(perlin, 0.0, 1.0, worley, 1.0);
    o.y = cl_worley_fbm(t * (ff * 2.0), F * 2u, P.seed + 20u);
    o.z = cl_worley_fbm(t * (ff * 4.0), F * 4u, P.seed + 30u);
    o.w = cl_worley_fbm(t * (ff * 8.0), F * 8u, P.seed + 40u);
    return o;
}

float4 cl_detail_texel(CloudParams P, uint3 i) {
    const float3 t = cl_tile_coord(i, P.detailSize);
    const uint F = P.detailFrequency;
    const float ff = float(F);
    float4 o;
    o.x = cl_worley_fbm(t * ff, F, P.seed + 50u);
    o.y = cl_worley_fbm(t * (ff * 2.0), F * 2u, P.seed + 60u);
    o.z = cl_worley_fbm(t * (ff * 4.0), F * 4u, P.seed + 70u);
    o.w = 0.0;
    return o;
}

float4 cl_weather_texel(CloudParams P, uint x, uint y) {
    const float inv = 1.0 / float(P.weatherSize);
    const uint F = P.weatherFrequency;
    const float ff = float(F);
    const float u = (float(x) + 0.5) * inv * ff;
    const float v = (float(y) + 0.5) * inv * ff;
    const float3 q = float3(u, v, 0.5);
    const float3 q2 = float3(u * 2.0, v * 2.0, 0.5);
    float4 o;
    o.x = cl_saturate(0.5 + 1.2 * cl_perlin_fbm(q, F, P.seed + 80u));
    o.y = 0.5 + 0.5 * cl_worley(q2, F * 2u, P.seed + 90u);
    o.z = cl_saturate(0.5 + 1.5 * cl_perlin_fbm(q, F, P.seed + 100u));
    o.w = 0.0;
    return o;
}

// --- sampling -------------------------------------------------------------------------------------------------
void cl_wrap_axis(float c, uint n, out uint i0, out uint i1, out float t) {
    const float f = c * float(n) - 0.5;
    const float fl = floor(f);
    t = f - fl;
    const float nf = float(n);
    const float w = fl - nf * floor(fl / nf);
    i0 = min(uint(w), n - 1u);
    i1 = i0 + 1u == n ? 0u : i0 + 1u;
}

float4 cl_sample3(uint64_t volume, uint n, float cx, float cy, float cz) {
    uint x0, x1, y0, y1, z0, z1;
    float tx, ty, tz;
    cl_wrap_axis(cx, n, x0, x1, tx);
    cl_wrap_axis(cy, n, y0, y1, ty);
    cl_wrap_axis(cz, n, z0, z1, tz);
    float4* V = cl_texels(volume);
    const float4 a = V[(z0 * n + y0) * n + x0];
    const float4 b = V[(z0 * n + y0) * n + x1];
    const float4 c = V[(z0 * n + y1) * n + x0];
    const float4 d = V[(z0 * n + y1) * n + x1];
    const float4 e = V[(z1 * n + y0) * n + x0];
    const float4 f = V[(z1 * n + y0) * n + x1];
    const float4 g = V[(z1 * n + y1) * n + x0];
    const float4 h = V[(z1 * n + y1) * n + x1];
    const float4 ab = a + (b - a) * tx;
    const float4 cd = c + (d - c) * tx;
    const float4 ef = e + (f - e) * tx;
    const float4 gh = g + (h - g) * tx;
    const float4 lo = ab + (cd - ab) * ty;
    const float4 hi = ef + (gh - ef) * ty;
    return lo + (hi - lo) * tz;
}

float4 cl_sample2(uint64_t table, uint n, float cx, float cy) {
    uint x0, x1, y0, y1;
    float tx, ty;
    cl_wrap_axis(cx, n, x0, x1, tx);
    cl_wrap_axis(cy, n, y0, y1, ty);
    float4* V = cl_texels(table);
    const float4 a = V[y0 * n + x0];
    const float4 b = V[y0 * n + x1];
    const float4 c = V[y1 * n + x0];
    const float4 d = V[y1 * n + x1];
    const float4 ab = a + (b - a) * tx;
    const float4 cd = c + (d - c) * tx;
    return ab + (cd - ab) * ty;
}

/// Clamped bilinear fetch of a w x h table with `stride` texels per pixel, component k.
float4 cl_bilinear_clamp(uint64_t table, uint w, uint h, uint stride, uint k, float fx, float fy) {
    const float cx = max(0.0, min(fx, float(w - 1u)));
    const float cy = max(0.0, min(fy, float(h - 1u)));
    const uint x0 = min(uint(cx), w - 2u);
    const uint y0 = min(uint(cy), h - 2u);
    const float tx = cx - float(x0);
    const float ty = cy - float(y0);
    float4* V = cl_texels(table);
    const float4 a = V[(y0 * w + x0) * stride + k];
    const float4 b = V[(y0 * w + x0 + 1u) * stride + k];
    const float4 c = V[((y0 + 1u) * w + x0) * stride + k];
    const float4 d = V[((y0 + 1u) * w + x0 + 1u) * stride + k];
    const float4 ab = a + (b - a) * tx;
    const float4 cd = c + (d - c) * tx;
    return ab + (cd - ab) * ty;
}

/// Catmull-Rom (4 x 4 taps, clamped to the texture) of component k, then clamped to the 2 x 2 bilinear footprint
/// (no ringing): a sharp history fetch that does not blur under repeated reprojection.
float4 cl_catmull_rom(uint64_t table, uint w, uint h, uint stride, uint k, float fx, float fy) {
    const float cx = max(0.0, min(fx, float(w - 1u)));
    const float cy = max(0.0, min(fy, float(h - 1u)));
    const float flx = floor(cx);
    const float fly = floor(cy);
    const float tx = cx - flx;
    const float ty = cy - fly;
    const int x0 = int(flx);
    const int y0 = int(fly);
    float wx[4];
    float wy[4];
    wx[0] = tx * (-0.5 + tx * (1.0 - 0.5 * tx));
    wx[1] = 1.0 + tx * tx * (-2.5 + 1.5 * tx);
    wx[2] = tx * (0.5 + tx * (2.0 - 1.5 * tx));
    wx[3] = tx * tx * (-0.5 + 0.5 * tx);
    wy[0] = ty * (-0.5 + ty * (1.0 - 0.5 * ty));
    wy[1] = 1.0 + ty * ty * (-2.5 + 1.5 * ty);
    wy[2] = ty * (0.5 + ty * (2.0 - 1.5 * ty));
    wy[3] = ty * ty * (-0.5 + 0.5 * ty);
    float4* V = cl_texels(table);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float4 lo = float4(1e30, 1e30, 1e30, 1e30);
    float4 hi = float4(-1e30, -1e30, -1e30, -1e30);
    for (int j = 0; j < 4; ++j) {
        const uint yy = uint(min(max(y0 - 1 + j, 0), int(h) - 1));
        float4 row = float4(0.0, 0.0, 0.0, 0.0);
        for (int i = 0; i < 4; ++i) {
            const uint xx = uint(min(max(x0 - 1 + i, 0), int(w) - 1));
            const float4 t = V[(yy * w + xx) * stride + k];
            row = row + t * wx[i];
            if ((i == 1 || i == 2) && (j == 1 || j == 2)) {
                lo = min(lo, t);
                hi = max(hi, t);
            }
        }
        sum = sum + row * wy[j];
    }
    return max(lo, min(sum, hi));
}

// --- density ----------------------------------------------------------------------------------------------------
float cl_height_gradient(float h, float type) {
    const float t2 = cl_saturate(type * 2.0);
    const float t3 = cl_saturate(type * 2.0 - 1.0);
    const float a = cl_lerp(cl_lerp(0.00, 0.00, t2), 0.00, t3);
    const float b = cl_lerp(cl_lerp(0.10, 0.15, t2), 0.10, t3);
    const float c = cl_lerp(cl_lerp(0.15, 0.40, t2), 0.70, t3);
    const float d = cl_lerp(cl_lerp(0.30, 0.65, t2), 1.00, t3);
    return cl_saturate((h - a) / max(1e-4, b - a)) * cl_saturate((d - h) / max(1e-4, d - c));
}

float cl_density(CloudParams P, float3 pos, float h) {
    if (h < 0.0 || h > 1.0) {
        return 0.0;
    }
    if ((P.flags & CL_FLAG_HOMOGENEOUS) != 0u) {
        return P.densityScale;
    }
    const float wx = pos.x - P.windOffset[0];
    const float wy = pos.y - P.windOffset[1];
    const float wz = pos.z - P.windOffset[2];
    const float4 W = cl_sample2(P.weather, P.weatherSize, wx * P.weatherScale, wz * P.weatherScale);
    const float coverage = cl_saturate(W.x * P.coverageScale + P.coverageBias);
    if (coverage <= 0.0) {
        return 0.0;
    }
    const float4 S = cl_sample3(P.shapeNoise, P.shapeSize, wx * P.shapeScale, wy * P.shapeScale, wz * P.shapeScale);
    const float low = S.y * 0.625 + S.z * 0.25 + S.w * 0.125;
    float base = cl_remap(S.x, low - 1.0, 1.0, 0.0, 1.0);
    base = cl_saturate(base * cl_height_gradient(h, W.z));
    base = cl_saturate(cl_remap(base, 1.0 - coverage, 1.0, 0.0, 1.0)) * coverage;
    if (base <= 0.0) {
        return 0.0;
    }
    const float4 D = cl_sample3(P.detailNoise, P.detailSize, wx * P.detailScale, wy * P.detailScale, wz * P.detailScale);
    const float hf = D.x * 0.625 + D.y * 0.25 + D.z * 0.125;
    const float mod_ = cl_lerp(hf, 1.0 - hf, cl_saturate(h * 10.0));
    const float dens = cl_saturate(cl_remap(base, mod_ * P.detailStrength, 1.0, 0.0, 1.0));
    return dens * W.y * P.densityScale;
}

float cl_altitude(CloudParams P, float3 pos, out float r, out float3 up) {
    const float R = P.planetRadius;
    const float yc = pos.y + R;
    const float rr = sqrt(pos.x * pos.x + yc * yc + pos.z * pos.z);
    const float alt = (pos.x * pos.x + pos.z * pos.z + pos.y * (pos.y + 2.0 * R)) / (rr + R);
    const float inv = 1.0 / rr;
    up = float3(pos.x * inv, yc * inv, pos.z * inv);
    r = R + alt;
    return alt;
}

float cl_density_at(CloudParams P, float3 pos) {
    float r;
    float3 up;
    const float alt = cl_altitude(P, pos, r, up);
    const float h = (alt - P.cloudBottom) / (P.cloudTop - P.cloudBottom);
    return cl_density(P, pos, h);
}

// --- geometry ----------------------------------------------------------------------------------------------------
bool cl_sphere(CloudParams P, float alt, float r, float mu, float H, out float t0, out float t1) {
    t0 = 0.0;
    t1 = 0.0;
    const float c = (alt - H) * (r + P.planetRadius + H);
    const float b = r * mu;
    const float disc = b * b - c;
    if (disc < 0.0) {
        return false;
    }
    const float s = sqrt(disc);
    const float q = b >= 0.0 ? -(b + s) : s - b;
    if (q == 0.0) {
        return false;
    }
    const float ta = q;
    const float tb = c / q;
    t0 = min(ta, tb);
    t1 = max(ta, tb);
    return true;
}

float cl_height(CloudParams P, float alt, float r, float mu, float t) {
    const float R = P.planetRadius;
    const float k = t * (2.0 * r * mu + t);
    const float numerator = alt * (r + R) + k;
    const float rt = sqrt(max(0.0, r * r + k));
    return numerator / (rt + R);
}

bool cl_segment(CloudParams P, float alt, float r, float mu, out float tStart, out float tEnd) {
    tStart = 0.0;
    tEnd = 0.0;
    float a0, a1, b0, b1;
    const bool top = cl_sphere(P, alt, r, mu, P.cloudTop, a0, a1);
    const bool bottom = cl_sphere(P, alt, r, mu, P.cloudBottom, b0, b1);
    if (!top) {
        return false;
    }
    float start = 0.0;
    float end_ = 0.0;
    if (alt < P.cloudBottom) {
        float g0, g1;
        if (cl_sphere(P, alt, r, mu, 0.0, g0, g1) && g0 > 0.0) {
            return false;
        }
        if (!bottom) {
            return false;
        }
        start = b1;
        end_ = a1;
    } else if (alt <= P.cloudTop) {
        start = 0.0;
        end_ = (bottom && b0 > 0.0) ? b0 : a1;
    } else {
        if (a1 <= 0.0) {
            return false;
        }
        start = max(a0, 0.0);
        end_ = (bottom && b0 > 0.0) ? b0 : a1;
    }
    end_ = min(end_, P.maxDistance);
    tStart = start;
    tEnd = end_;
    return end_ > start;
}

// --- lighting and integration ----------------------------------------------------------------------------------------
float cl_hg(float cosTheta, float g) {
    const float g2 = g * g;
    const float base = max(1e-6, 1.0 + g2 - 2.0 * g * cosTheta);
    return (1.0 - g2) / (4.0 * CL_PI * base * sqrt(base));
}

float cl_phase(CloudParams P, float cosTheta, float eccentricity) {
    return cl_lerp(cl_hg(cosTheta, P.phaseForward * eccentricity), cl_hg(cosTheta, P.phaseBackward * eccentricity),
                   P.phaseBlend);
}

float cl_sun_transfer(CloudParams P, float cosTheta, float tau) {
    const float powder = cl_one_minus_exp(2.0 * tau);
    const float pm = cl_lerp(1.0, min(1.0, 2.0 * powder), P.powderStrength * cl_saturate(0.5 - 0.5 * cosTheta));
    float sum = 0.0;
    float a = 1.0;
    float b = 1.0;
    float c = 1.0;
    for (uint n = 0u; n < P.octaves; ++n) {
        sum += a * cl_phase(P, cosTheta, c) * exp(-b * tau);
        a *= P.msAttenuation;
        b *= P.msExtinction;
        c *= P.msEccentricity;
    }
    return sum * pm;
}

float cl_light_depth(CloudParams P, float3 pos, float alt, float3 up) {
    const float3 L = cl_v3(P.sunDir);
    const float r = P.planetRadius + alt;
    const float mu = cl_dot(L, up);
    float t0, t1;
    if (!cl_sphere(P, alt, r, mu, P.cloudTop, t0, t1) || t1 <= 0.0) {
        return 0.0;
    }
    const float len = min(t1, P.lightDistance);
    const float ds = len / float(P.lightSteps);
    const float span = P.cloudTop - P.cloudBottom;
    float tau = 0.0;
    for (uint j = 0u; j < P.lightSteps; ++j) {
        const float s = (float(j) + 0.5) * ds;
        const float h = (cl_height(P, alt, r, mu, s) - P.cloudBottom) / span;
        const float3 q = pos + L * s;
        tau += cl_density(P, q, h) * ds;
    }
    return tau;
}

float3 cl_ambient_top(CloudParams P) {
    if ((P.flags & CL_FLAG_ATMOSPHERE) == 0u || P.atmosphere == 0) {
        return cl_v3(P.ambient);
    }
    const AtParams A = at_load(P.atmosphere);
    const float c = 0.8660254;
    const float s = 0.5;
    float3 sum = at_sky_radiance(A, float3(0.0, 1.0, 0.0), false);
    sum = sum + at_sky_radiance(A, float3(c, s, 0.0), false);
    sum = sum + at_sky_radiance(A, float3(-c, s, 0.0), false);
    sum = sum + at_sky_radiance(A, float3(0.0, s, c), false);
    sum = sum + at_sky_radiance(A, float3(0.0, s, -c), false);
    return sum * (0.2 * P.ambientScale);
}

struct ClRay {
    float3 scatter;
    float transmittance;
    float depth;
    float hasCloud;
};

ClRay cl_integrate(CloudParams P, float3 origin, float3 dir, float jitter) {
    ClRay o;
    o.scatter = float3(0.0);
    o.transmittance = 1.0;
    o.depth = P.maxDistance;
    o.hasCloud = 0.0;
    float r;
    float3 up;
    const float alt = cl_altitude(P, origin, r, up);
    const float mu = cl_dot(dir, up);
    float start, end_;
    if (!cl_segment(P, alt, r, mu, start, end_)) {
        return o;
    }
    const bool withAtm = (P.flags & CL_FLAG_ATMOSPHERE) != 0u && P.atmosphere != 0;
    const float dt = (end_ - start) / float(P.primarySteps);
    const float3 sun = cl_v3(P.sunDir);
    const float cosTheta = cl_dot(dir, sun);
    const float3 ambientTop = cl_ambient_top(P);
    const float3 sunConst = cl_v3(P.sunIlluminance);
    const float span = P.cloudTop - P.cloudBottom;
    AtParams A = {};
    if (withAtm) {
        A = at_load(P.atmosphere);
    }
    float3 L = float3(0.0);
    float T = 1.0;
    float dsum = 0.0;
    float wsum = 0.0;
    for (uint i = 0u; i < P.primarySteps; ++i) {
        const float t = start + (float(i) + jitter) * dt;
        const float altT = cl_height(P, alt, r, mu, t);
        const float h = (altT - P.cloudBottom) / span;
        const float3 pos = origin + dir * t;
        const float sigma = cl_density(P, pos, h);
        if (sigma <= 0.0) {
            continue;
        }
        float rs;
        float3 ups;
        const float altS = cl_altitude(P, pos, rs, ups);
        const float tau = cl_light_depth(P, pos, altS, ups);
        float3 E = sunConst;
        if (withAtm) {
            E = at_sun_illuminance_at(A, pos);
        }
        const float3 amb = ambientTop * cl_lerp(P.ambientBottom, 1.0, cl_saturate(h));
        const float3 X = E * cl_sun_transfer(P, cosTheta, tau) + amb;
        const float absorbed = cl_one_minus_exp(sigma * dt);
        L = L + X * (T * P.albedo * absorbed);
        const float Tn = T * (1.0 - absorbed);
        dsum += (T - Tn) * t;
        wsum += T - Tn;
        T = Tn;
        if (T < P.transmittanceCutoff) {
            break;
        }
    }
    o.scatter = L;
    o.transmittance = T;
    o.hasCloud = wsum > 1e-4 ? 1.0 : 0.0;
    o.depth = wsum > 1e-4 ? dsum / wsum : 0.5 * (start + end_);
    return o;
}

float3 cl_view_dir(CloudParams P, float u, float v) {
    const float sx = (2.0 * u - 1.0) * P.camForward[3];
    const float sy = (2.0 * v - 1.0) * P.camRight[3];
    const float3 d = cl_v3(P.camForward) + cl_v3(P.camRight) * sx - cl_v3(P.camUp) * sy;
    return d * (1.0 / sqrt(cl_dot(d, d)));
}

float cl_jitter(CloudParams P, uint x, uint y) {
    if ((P.flags & CL_FLAG_JITTER) == 0u) {
        return 0.5;
    }
    return cl_fract(cl_unit(cl_hash4(x, y, 0u, P.seed ^ 0x5bd1e995u)) + float(P.cycle) * 0.618034);
}

// --- texel kernels -------------------------------------------------------------------------------------------------
void cl_march_texel(CloudParams P, uint fx, uint fy, out float4 o0, out float4 o1) {
    const uint x = fx * P.block + P.offsetX;
    const uint y = fy * P.block + P.offsetY;
    const float u = (float(x) + 0.5) / float(P.width);
    const float v = (float(y) + 0.5) / float(P.height);
    const float3 dir = cl_view_dir(P, u, v);
    const ClRay ray = cl_integrate(P, cl_v3(P.cameraPos), dir, cl_jitter(P, x, y));
    o0 = float4(ray.scatter, ray.transmittance);
    o1 = float4(ray.depth, ray.hasCloud, 0.0, 0.0);
}

void cl_reconstruct_texel(CloudParams P, uint x, uint y, out float4 o0, out float4 o1) {
    const uint B = P.block;
    const uint bx = x / B;
    const uint by = y / B;
    float4* fresh = cl_texels(P.fresh);
    const float4 F0 = fresh[(by * P.freshWidth + bx) * 2u];
    const float4 F1 = fresh[(by * P.freshWidth + bx) * 2u + 1u];
    const bool isFresh = x - bx * B == P.offsetX && y - by * B == P.offsetY;
    const float4 fallback1 = float4(F1.x, isFresh ? 1.0 : 0.0, F1.y, 0.0);
    if ((P.flags & CL_FLAG_HISTORY) == 0u) {
        o0 = F0;
        o1 = fallback1;
        return;
    }
    const float W = float(P.width);
    const float H = float(P.height);
    const float u = (float(x) + 0.5) / W;
    const float v = (float(y) + 0.5) / H;
    float pu = u;
    float pv = v;
    bool inside = true;
    bool offscreen = false;
    float motion = 0.0;
    if ((P.flags & CL_FLAG_NO_REPROJECT) == 0u) {
        const float3 dir = cl_view_dir(P, u, v);
        const float3 world = cl_v3(P.cameraPos) + dir * F1.x - cl_v3(P.windDelta);
        const float3 rel = world - cl_v3(P.prevCameraPos);
        const float z = cl_dot(rel, cl_v3(P.prevForward));
        if (z <= 1e-3) {
            inside = false;
        } else {
            const float px = cl_dot(rel, cl_v3(P.prevRight)) / (z * P.prevForward[3]);
            const float py = cl_dot(rel, cl_v3(P.prevUp)) / (z * P.prevRight[3]);
            pu = 0.5 + 0.5 * px;
            pv = 0.5 - 0.5 * py;
            const float mx = (pu - u) * W;
            const float my = (pv - v) * H;
            motion = sqrt(mx * mx + my * my);
            // Off screen: the nearest edge history, clamped to the fresh neighbourhood below.
            offscreen = pu < 0.0 || pu > 1.0 || pv < 0.0 || pv > 1.0;
            pu = max(0.0, min(pu, 1.0));
            pv = max(0.0, min(pv, 1.0));
        }
    }
    if (!inside) {
        o0 = F0;
        o1 = fallback1;
        return;
    }
    const bool moving = motion > P.staticThreshold;
    float4 H0;
    float4 H1;
    if (!moving) {
        float4* hist = cl_texels(P.historyIn);
        H0 = hist[(y * P.width + x) * 2u];
        H1 = hist[(y * P.width + x) * 2u + 1u];
    } else {
        H0 = cl_catmull_rom(P.historyIn, P.width, P.height, 2u, 0u, pu * W - 0.5, pv * H - 0.5);
        H1 = cl_bilinear_clamp(P.historyIn, P.width, P.height, 2u, 1u, pu * W - 0.5, pv * H - 0.5);
        // History rejection: a history from another depth (disocclusion, a cloud edge moving over the sky) is
        // clamped to the fresh 3 x 3 block neighbourhood; a consistent one is kept sharp.
        const float expected = F1.x;
        if (offscreen || max(expected, H1.x) > P.depthRejection * min(expected, H1.x)) {
            float4 mn = F0;
            float4 mx = F0;
            for (uint k = 0u; k < 9u; ++k) {
                const uint nx = uint(min(max(int(bx) + int(k % 3u) - 1, 0), int(P.freshWidth) - 1));
                const uint ny = uint(min(max(int(by) + int(k / 3u) - 1, 0), int(P.freshHeight) - 1));
                const float4 n = fresh[(ny * P.freshWidth + nx) * 2u];
                mn = min(mn, n);
                mx = max(mx, n);
            }
            H0 = max(mn, min(H0, mx));
        }
    }
    float count = H1.y;
    if (moving) {
        count = min(count, P.motionHistoryCount);
    }
    if (isFresh) {
        const float n = min(count + 1.0, moving ? P.motionHistoryCount : P.maxHistoryCount);
        const float w = 1.0 / n;
        if (w >= 1.0) {
            o0 = F0; // the first sample replaces the fill value exactly
        } else {
            o0 = H0 + (F0 - H0) * w;
        }
        o1 = float4(F1.x, n, F1.y, 0.0);
    } else {
        o0 = H0;
        o1 = float4(H1.x, count, H1.z, 0.0);
    }
}

float4 cl_composite_texel(CloudParams P, uint x, uint y) {
    const float u = (float(x) + 0.5) / float(P.outWidth);
    const float v = (float(y) + 0.5) / float(P.outHeight);
    const float fx = u * float(P.width) - 0.5;
    const float fy = v * float(P.height) - 0.5;
    const float4 C0 = cl_bilinear_clamp(P.historyOut, P.width, P.height, 2u, 0u, fx, fy);
    const float4 C1 = cl_bilinear_clamp(P.historyOut, P.width, P.height, 2u, 1u, fx, fy);
    const bool withAtm = (P.flags & CL_FLAG_ATMOSPHERE) != 0u && P.atmosphere != 0;
    const float3 dir = cl_view_dir(P, u, v);
    float3 bg = float3(0.0);
    if ((P.flags & CL_FLAG_BACKGROUND) != 0u && P.background != 0) {
        bg = cl_texels(P.background)[y * P.outWidth + x].xyz;
    } else if (withAtm) {
        bg = at_sky_radiance(at_load(P.atmosphere), dir, (P.flags & CL_FLAG_SUN_DISK) != 0u);
    }
    if ((P.flags & CL_FLAG_SCENE_DEPTH) != 0u && P.depth != 0 && cl_floats(P.depth)[y * P.outWidth + x] < C1.x) {
        return float4(bg, 1.0);
    }
    const float T = C0.w;
    const float3 L = C0.xyz;
    float3 col;
    if (withAtm && (P.flags & CL_FLAG_AERIAL) != 0u) {
        const AtParams A = at_load(P.atmosphere);
        float3 s;
        float3 t;
        at_aerial(A, float2(u, v), C1.x, s, t);
        const float3 E = float3(A.sunIlluminance[0], A.sunIlluminance[1], A.sunIlluminance[2]);
        col = bg * T + L * t + (s * E) * (1.0 - T);
    } else {
        col = bg * T + L;
    }
    return float4(col, T);
}

