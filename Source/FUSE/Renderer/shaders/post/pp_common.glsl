// WP-4.5 GPU post stack: records, push constants and the per-pixel math every post kernel shares.
// GLSL twin of pp_common.slang. The C++ mirror of the records is
// include/fuse/renderer/postprocess/gpu/post_gpu_types.hpp; the CPU oracle of the math is
// renderer/postprocess/* (bloom.cpp, dof.cpp, motion_blur.cpp, auto_exposure.cpp, tonemap.cpp,
// tonemap_curve.cpp, color_grade.cpp) and look/look_kernels.hpp (LUT, look vignette / grain).
// Every expression keeps the oracle's operation order; values that feed comparisons or rounding are
// `precise` so no multiply-add is contracted.
#ifndef FUSE_PP_COMMON_GLSL
#define FUSE_PP_COMMON_GLSL
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_samplerless_texture_functions : require
#include "bindless.glsl"

#define PP_FLAG_SRGB (1u << 0)
#define PP_FLAG_AUTO_EXPOSURE (1u << 1)
#define PP_FLAG_AUTO_EMA (1u << 2)
#define PP_FLAG_EXPOSURE_RESET (1u << 3)
#define PP_FLAG_CURVE (1u << 4)
#define PP_FLAG_GRADE_LIFT_CONTRAST (1u << 5)
#define PP_FLAG_GRADE_SATURATION (1u << 6)
#define PP_FLAG_GRADE_GAMMA_GAIN (1u << 7)
#define PP_FLAG_LUT (1u << 8)
#define PP_FLAG_VIGNETTE_STACK (1u << 9)
#define PP_FLAG_VIGNETTE_LOOK (1u << 10)
#define PP_FLAG_GRAIN_STACK (1u << 11)
#define PP_FLAG_GRAIN_LOOK (1u << 12)
#define PP_FLAG_MB_DEPTH (1u << 13)
#define PP_FLAG_DOF_NEAR_BLUR (1u << 14)
#define PP_FLAG_GRADE_CLAMP (1u << 15)

#define PP_TONE_ACES 0u
#define PP_TONE_FILMIC 1u
#define PP_TONE_REINHARD 2u
#define PP_TONE_NEUTRAL 3u
#define PP_TONE_AGX 4u

#define PP_CURVE_FILMIC 0u
#define PP_CURVE_REINHARD 1u
#define PP_CURVE_ACES 2u

#define PP_HIST_TILE 64u
#define PP_HIST_MAX_BINS 256u

// PostFrameConstants, 448 bytes.
struct PpFrame {
    uint64_t histPartials;
    uint64_t exposureState;
    uint64_t lut;
    uint64_t dump;
    uint64_t cocs;
    uint64_t tileMax;
    uint64_t neighborMax;
    uint64_t reserved0;
    uint width;
    uint height;
    uint inputHdr;
    uint inputDepth;
    uint inputVelocity;
    uint output_;
    uint flags;
    uint toneMapper;
    float exposureScale;
    float exposureEv;
    float histMinLog;
    float histMaxLog;
    uint histBins;
    uint histGroupsX;
    uint histGroups;
    float percentile;
    float minEv;
    float maxEv;
    float targetLuminance;
    float meteringBias;
    float speedUp;
    float speedDown;
    float emaUp;
    float emaDown;
    float deltaSeconds;
    float resetEv;
    uint adaptValid;
    uint histValid;
    float bloomThreshold;
    float bloomKnee;
    float bloomIntensity;
    float bloomScatter;
    float bloomTint[4];
    float dofFocalDistance;
    float dofFocalLength;
    float dofFStop;
    float dofSensorWidth;
    float dofMaxRadius;
    uint dofReach;
    uint reserved1;
    uint reserved2;
    uint mbSamples;
    float mbShutter;
    float mbMaxBlur;
    float mbSoftDepth;
    uint mbTile;
    uint mbTilesX;
    uint mbTilesY;
    uint reserved3;
    uint curveKind;
    float filmicA;
    float filmicB;
    float filmicC;
    float filmicD;
    float filmicE;
    float filmicF;
    float filmicWhiteScale;
    float curveGamma;
    float reinhardWhite;
    float reinhardScale;
    float acesContrast;
    float acesShoulder;
    uint reserved4;
    uint reserved5;
    uint lutSize;
    float lift[4];
    float invGamma[4];
    float gain[4];
    float vignetteTint[4];
    float contrast;
    float saturation;
    float vignette;
    float vignetteFalloff;
    float vignetteRoundness;
    float grain;
    float grainResponse;
    uint seedLo;
    uint seedHi;
    uint reserved6;
    uint reserved7;
    uint reserved8;
};

// PostExposureState: 8 words + bins.
#define PP_STATE_CURRENT_EV 0u
#define PP_STATE_MEASURED 1u
#define PP_STATE_SMOOTHED 2u
#define PP_STATE_METERED 3u
#define PP_STATE_SAMPLES 4u
#define PP_STATE_FRAMES 5u
#define PP_STATE_BINS 8u

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PpFrameRef { PpFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer PpTexelsRef { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer PpPairsRef { vec2 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer PpWordsRef { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer PpFloatsRef { float v[]; };

layout(push_constant) uniform PpPush {
    uint64_t frame;
    uint64_t src;
    uint64_t dst;
    uint64_t aux;
    uint mode;
    uint srcW;
    uint srcH;
    uint dstW;
    uint dstH;
    uint reserved0;
    uint reserved1;
    uint reserved2;
} pc;

PpFrame pp_frame() { return PpFrameRef(pc.frame).f; }

vec4 pp_fetch(uint handle, ivec2 p) { return texelFetch(fuse_textures_2d[fuse_handle_index(handle)], p, 0); }

// --- scalar helpers (std::clamp / std::min / std::max semantics on finite values) ----------------
float pp_clamp(float v, float lo, float hi) { return min(max(v, lo), hi); }
float pp_saturate(float v) { return min(max(v, 0.0), 1.0); }
float pp_luminance(vec3 c) {
    precise float l = 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
    return l;
}
// std::lround for the magnitudes used here (exact: v - trunc(v) is exact in IEEE arithmetic).
int pp_lround(float v) {
    const float t = trunc(v);
    precise float frac = abs(v - t);
    return int(t) + (frac >= 0.5 ? (v < 0.0 ? -1 : 1) : 0);
}
// std::round of a non-negative value.
uint pp_round_pos(float v) {
    const float t = trunc(v);
    precise float frac = v - t;
    return uint(t) + (frac >= 0.5 ? 1u : 0u);
}
uint pp_clamp_index(int i, uint size) { return i < 0 ? 0u : (i >= int(size) ? size - 1u : uint(i)); }

// --- tone mapping (renderer/postprocess/tonemap.cpp) ---------------------------------------------
float pp_aces(float v) {
    precise float n = v * (2.51 * v + 0.03);
    precise float d = v * (2.43 * v + 0.59) + 0.14;
    return pp_clamp(n / d, 0.0, 1.0);
}
float pp_filmic(float v) {
    const float A = 0.22, B = 0.30, C = 0.10, D = 0.20, E = 0.01, F = 0.30;
    precise float n = v * (A * v + C * B) + D * E;
    precise float d = v * (A * v + B) + D * F;
    precise float r = n / d - E / F;
    return pp_clamp(r, 0.0, 1.0);
}
float pp_reinhard(float v) { return pp_clamp(v / (1.0 + v), 0.0, 1.0); }

float pp_agx_contrast(float x) {
    precise float x2 = x * x;
    precise float x4 = x2 * x2;
    precise float r = ((((((15.5 * x4) * x2 - (40.14 * x4) * x) + 31.96 * x4) - (6.868 * x2) * x) + 0.4298 * x2) + 0.1191 * x) - 0.00232;
    return r;
}
float pp_agx_encode(float v) {
    const float kMin = -12.47393;
    const float kMax = 4.026069;
    const float ev = pp_clamp(log2(max(v, 1e-10)), kMin, kMax);
    precise float t = (ev - kMin) / (kMax - kMin);
    return pp_agx_contrast(t);
}
float pp_agx_decode(float v) { return pp_clamp(pow(max(v, 0.0), 2.2), 0.0, 1.0); }
vec3 pp_agx(vec3 x) {
    precise float r = (0.842479062253094 * x.x + 0.0784335999999992 * x.y) + 0.0792237451477643 * x.z;
    precise float g = (0.0423282422610123 * x.x + 0.878468636469772 * x.y) + 0.0791661274605434 * x.z;
    precise float b = (0.0423756549057051 * x.x + 0.0784336 * x.y) + 0.879142973793104 * x.z;
    const float er = pp_agx_encode(r);
    const float eg = pp_agx_encode(g);
    const float eb = pp_agx_encode(b);
    precise float orr = (1.19687900512017 * er - 0.0980208811401368 * eg) - 0.0990297440797205 * eb;
    precise float og = (-0.0528968517574562 * er + 1.15190312990417 * eg) - 0.0989611768448433 * eb;
    precise float ob = (-0.0529716355144438 * er - 0.0980434501171241 * eg) + 1.15107367264116 * eb;
    return vec3(pp_agx_decode(orr), pp_agx_decode(og), pp_agx_decode(ob));
}

vec3 pp_tonemap(vec3 c, uint op) {
    if (op == PP_TONE_AGX) {
        return pp_agx(c);
    }
    if (op == PP_TONE_FILMIC) {
        return vec3(pp_filmic(c.x), pp_filmic(c.y), pp_filmic(c.z));
    }
    if (op == PP_TONE_REINHARD) {
        return vec3(pp_reinhard(c.x), pp_reinhard(c.y), pp_reinhard(c.z));
    }
    if (op == PP_TONE_NEUTRAL) {
        return vec3(pp_saturate(c.x), pp_saturate(c.y), pp_saturate(c.z));
    }
    return vec3(pp_aces(c.x), pp_aces(c.y), pp_aces(c.z));
}

// --- pre-tonemap curve (renderer/postprocess/tonemap_curve.cpp) ----------------------------------
float pp_filmic_segment(float x, PpFrame F) {
    precise float n = x * (F.filmicA * x + F.filmicC * F.filmicB) + F.filmicD * F.filmicE;
    precise float d = x * (F.filmicA * x + F.filmicB) + F.filmicD * F.filmicF;
    precise float r = n / d - F.filmicE / F.filmicF;
    return r;
}
float pp_curve_channel(float c, PpFrame F) {
    if (F.curveKind == PP_CURVE_REINHARD) {
        precise float e = max(c, 0.0) * F.reinhardScale;
        precise float n = e * (1.0 + e / (F.reinhardWhite * F.reinhardWhite));
        precise float d = 1.0 + e;
        return pp_clamp(n / d, 0.0, 1.0);
    }
    if (F.curveKind == PP_CURVE_ACES) {
        precise float s = max(c, 0.0) * F.acesContrast;
        precise float m = (s * (2.51 * s + 0.03)) / (s * (2.43 * s + 0.59) + 0.14);
        precise float sb = m / (1.0 + F.acesShoulder * m);
        return pp_clamp(sb, 0.0, 1.0);
    }
    precise float mapped = pp_filmic_segment(c, F) * F.filmicWhiteScale;
    if (F.curveGamma != 1.0) {
        mapped = pow(max(mapped, 0.0), 1.0 / F.curveGamma);
    }
    return pp_clamp(mapped, 0.0, 1.0);
}

// --- sRGB (renderer::linear_to_srgb / look::kernels::srgb_encode) ------------------------------
float pp_srgb_encode(float c) {
    if (c <= 0.0031308) {
        return 12.92 * c;
    }
    precise float e = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return e;
}
float pp_srgb_decode(float e) {
    if (e <= 0.04045) {
        return e / 12.92;
    }
    return pow((e + 0.055) / 1.055, 2.4);
}

// --- 3D LUT (look::kernels::lut_sample_tetrahedral), f32x4 lattice, red fastest -------------------
vec3 pp_lut_at(PpTexelsRef lut, uint r, uint g, uint b, uint n) { return lut.v[r + g * n + b * n * n].xyz; }
vec3 pp_lut_tetrahedral(uint64_t address, uint n, vec3 c) {
    PpTexelsRef lut = PpTexelsRef(address);
    const float scale = float(n - 1u);
    precise float fr = pp_saturate(c.x) * scale;
    precise float fg = pp_saturate(c.y) * scale;
    precise float fb = pp_saturate(c.z) * scale;
    const uint r0 = min(uint(fr), n - 2u);
    const uint g0 = min(uint(fg), n - 2u);
    const uint b0 = min(uint(fb), n - 2u);
    precise float dr = fr - float(r0);
    precise float dg = fg - float(g0);
    precise float db = fb - float(b0);
    const vec3 c000 = pp_lut_at(lut, r0, g0, b0, n);
    const vec3 c111 = pp_lut_at(lut, r0 + 1u, g0 + 1u, b0 + 1u, n);
    precise vec3 result;
    if (dr > dg) {
        if (dg > db) {
            const vec3 c100 = pp_lut_at(lut, r0 + 1u, g0, b0, n);
            const vec3 c110 = pp_lut_at(lut, r0 + 1u, g0 + 1u, b0, n);
            result = ((c000 + (c100 - c000) * dr) + (c110 - c100) * dg) + (c111 - c110) * db;
        } else if (dr > db) {
            const vec3 c100 = pp_lut_at(lut, r0 + 1u, g0, b0, n);
            const vec3 c101 = pp_lut_at(lut, r0 + 1u, g0, b0 + 1u, n);
            result = ((c000 + (c100 - c000) * dr) + (c101 - c100) * db) + (c111 - c101) * dg;
        } else {
            const vec3 c001 = pp_lut_at(lut, r0, g0, b0 + 1u, n);
            const vec3 c101 = pp_lut_at(lut, r0 + 1u, g0, b0 + 1u, n);
            result = ((c000 + (c001 - c000) * db) + (c101 - c001) * dr) + (c111 - c101) * dg;
        }
    } else if (db > dg) {
        const vec3 c001 = pp_lut_at(lut, r0, g0, b0 + 1u, n);
        const vec3 c011 = pp_lut_at(lut, r0, g0 + 1u, b0 + 1u, n);
        result = ((c000 + (c001 - c000) * db) + (c011 - c001) * dg) + (c111 - c011) * dr;
    } else if (db > dr) {
        const vec3 c010 = pp_lut_at(lut, r0, g0 + 1u, b0, n);
        const vec3 c011 = pp_lut_at(lut, r0, g0 + 1u, b0 + 1u, n);
        result = ((c000 + (c010 - c000) * dg) + (c011 - c010) * db) + (c111 - c011) * dr;
    } else {
        const vec3 c010 = pp_lut_at(lut, r0, g0 + 1u, b0, n);
        const vec3 c110 = pp_lut_at(lut, r0 + 1u, g0 + 1u, b0, n);
        result = ((c000 + (c010 - c000) * dg) + (c110 - c010) * dr) + (c111 - c110) * db;
    }
    return result;
}

// --- film grain (renderer::film_grain_noise; bit-exact: integer hash, 24-bit float) -------------
uint64_t pp_splitmix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ul;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ul;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBul;
    return value ^ (value >> 31);
}
float pp_grain_noise(PpFrame F, uint px, uint py) {
    const uint64_t seed = (uint64_t(F.seedHi) << 32) | uint64_t(F.seedLo);
    const uint64_t pixel = (uint64_t(py) << 32) | uint64_t(px);
    const uint64_t hash = pp_splitmix64(pp_splitmix64(seed) ^ pixel);
    precise float unit = (float(uint(hash >> 40)) + 0.5) * (1.0 / 16777216.0);
    precise float r = unit * 2.0 - 1.0;
    return r;
}

// --- motion blur helpers (renderer/postprocess/motion_blur.cpp) ---------------------------------
float pp_length2(vec2 v) {
    precise float d = v.x * v.x + v.y * v.y;
    return sqrt(d);
}
float pp_dot2(vec2 v) {
    precise float d = v.x * v.x + v.y * v.y;
    return d;
}
vec2 pp_half_blur(vec2 velocity, PpFrame F) {
    precise vec2 blur = velocity * F.mbShutter;
    const float len = pp_length2(blur);
    if (len > F.mbMaxBlur && len > 0.0) {
        blur = blur * (F.mbMaxBlur / len);
    }
    precise vec2 h = blur * 0.5;
    return h;
}

#endif
