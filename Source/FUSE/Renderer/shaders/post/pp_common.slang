// WP-4.5 GPU post stack: Slang twin of pp_common.glsl (see there). Compile with -fp-mode precise
// (Slang does not honour `precise` per variable), so no multiply-add is contracted.
#pragma once

static const uint kPpFlagSrgb = 1u << 0;
static const uint kPpFlagAutoExposure = 1u << 1;
static const uint kPpFlagAutoEma = 1u << 2;
static const uint kPpFlagExposureReset = 1u << 3;
static const uint kPpFlagCurve = 1u << 4;
static const uint kPpFlagGradeLiftContrast = 1u << 5;
static const uint kPpFlagGradeSaturation = 1u << 6;
static const uint kPpFlagGradeGammaGain = 1u << 7;
static const uint kPpFlagLut = 1u << 8;
static const uint kPpFlagVignetteStack = 1u << 9;
static const uint kPpFlagVignetteLook = 1u << 10;
static const uint kPpFlagGrainStack = 1u << 11;
static const uint kPpFlagGrainLook = 1u << 12;
static const uint kPpFlagMbDepth = 1u << 13;
static const uint kPpFlagDofNearBlur = 1u << 14;
static const uint kPpFlagGradeClamp = 1u << 15;

static const uint kPpToneAces = 0u;
static const uint kPpToneFilmic = 1u;
static const uint kPpToneReinhard = 2u;
static const uint kPpToneNeutral = 3u;
static const uint kPpToneAgx = 4u;

static const uint kPpCurveFilmic = 0u;
static const uint kPpCurveReinhard = 1u;
static const uint kPpCurveAces = 2u;

static const uint kPpHistTile = 64u;
static const uint kPpHistMaxBins = 256u;

static const uint kPpStateCurrentEv = 0u;
static const uint kPpStateMeasured = 1u;
static const uint kPpStateSmoothed = 2u;
static const uint kPpStateMetered = 3u;
static const uint kPpStateSamples = 4u;
static const uint kPpStateFrames = 5u;
static const uint kPpStateBins = 8u;

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

struct PpPush {
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
};
[[vk::push_constant]] ConstantBuffer<PpPush> pc;

[[vk::binding(1, 0)]] Texture2D<float4> fuse_pp_textures[];

PpFrame ppFrame() { return ((PpFrame*)pc.frame)[0]; }
float4* ppTexels(uint64_t address) { return (float4*)address; }
float2* ppPairs(uint64_t address) { return (float2*)address; }
uint* ppWords(uint64_t address) { return (uint*)address; }
float* ppFloats(uint64_t address) { return (float*)address; }

float4 ppFetch(uint handle, int2 p) { return fuse_pp_textures[handle & 0xFFFFFu].Load(int3(p, 0)); }

// --- scalar helpers ---------------------------------------------------------------------------------
float ppClamp(float v, float lo, float hi) { return min(max(v, lo), hi); }
float ppSaturate(float v) { return min(max(v, 0.0), 1.0); }
float ppLuminance(float3 c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }
int ppLround(float v) {
    const float t = trunc(v);
    const float frac = abs(v - t);
    return int(t) + (frac >= 0.5 ? (v < 0.0 ? -1 : 1) : 0);
}
uint ppRoundPos(float v) {
    const float t = trunc(v);
    const float frac = v - t;
    return uint(t) + (frac >= 0.5 ? 1u : 0u);
}
uint ppClampIndex(int i, uint size) { return i < 0 ? 0u : (i >= int(size) ? size - 1u : uint(i)); }

// --- tone mapping ---------------------------------------------------------------------------------
float ppAces(float v) {
    const float n = v * (2.51 * v + 0.03);
    const float d = v * (2.43 * v + 0.59) + 0.14;
    return ppClamp(n / d, 0.0, 1.0);
}
float ppFilmic(float v) {
    const float A = 0.22, B = 0.30, C = 0.10, D = 0.20, E = 0.01, F = 0.30;
    const float n = v * (A * v + C * B) + D * E;
    const float d = v * (A * v + B) + D * F;
    return ppClamp(n / d - E / F, 0.0, 1.0);
}
float ppReinhard(float v) { return ppClamp(v / (1.0 + v), 0.0, 1.0); }

float ppAgxContrast(float x) {
    const float x2 = x * x;
    const float x4 = x2 * x2;
    return ((((((15.5 * x4) * x2 - (40.14 * x4) * x) + 31.96 * x4) - (6.868 * x2) * x) + 0.4298 * x2) + 0.1191 * x) - 0.00232;
}
float ppAgxEncode(float v) {
    const float kMin = -12.47393;
    const float kMax = 4.026069;
    const float ev = ppClamp(log2(max(v, 1e-10)), kMin, kMax);
    return ppAgxContrast((ev - kMin) / (kMax - kMin));
}
float ppAgxDecode(float v) { return ppClamp(pow(max(v, 0.0), 2.2), 0.0, 1.0); }
float3 ppAgx(float3 x) {
    const float r = (0.842479062253094 * x.x + 0.0784335999999992 * x.y) + 0.0792237451477643 * x.z;
    const float g = (0.0423282422610123 * x.x + 0.878468636469772 * x.y) + 0.0791661274605434 * x.z;
    const float b = (0.0423756549057051 * x.x + 0.0784336 * x.y) + 0.879142973793104 * x.z;
    const float er = ppAgxEncode(r);
    const float eg = ppAgxEncode(g);
    const float eb = ppAgxEncode(b);
    const float orr = (1.19687900512017 * er - 0.0980208811401368 * eg) - 0.0990297440797205 * eb;
    const float og = (-0.0528968517574562 * er + 1.15190312990417 * eg) - 0.0989611768448433 * eb;
    const float ob = (-0.0529716355144438 * er - 0.0980434501171241 * eg) + 1.15107367264116 * eb;
    return float3(ppAgxDecode(orr), ppAgxDecode(og), ppAgxDecode(ob));
}

float3 ppTonemap(float3 c, uint op) {
    if (op == kPpToneAgx) {
        return ppAgx(c);
    }
    if (op == kPpToneFilmic) {
        return float3(ppFilmic(c.x), ppFilmic(c.y), ppFilmic(c.z));
    }
    if (op == kPpToneReinhard) {
        return float3(ppReinhard(c.x), ppReinhard(c.y), ppReinhard(c.z));
    }
    if (op == kPpToneNeutral) {
        return float3(ppSaturate(c.x), ppSaturate(c.y), ppSaturate(c.z));
    }
    return float3(ppAces(c.x), ppAces(c.y), ppAces(c.z));
}

// --- pre-tonemap curve ------------------------------------------------------------------------------
float ppFilmicSegment(float x, PpFrame F) {
    const float n = x * (F.filmicA * x + F.filmicC * F.filmicB) + F.filmicD * F.filmicE;
    const float d = x * (F.filmicA * x + F.filmicB) + F.filmicD * F.filmicF;
    return n / d - F.filmicE / F.filmicF;
}
float ppCurveChannel(float c, PpFrame F) {
    if (F.curveKind == kPpCurveReinhard) {
        const float e = max(c, 0.0) * F.reinhardScale;
        const float n = e * (1.0 + e / (F.reinhardWhite * F.reinhardWhite));
        const float d = 1.0 + e;
        return ppClamp(n / d, 0.0, 1.0);
    }
    if (F.curveKind == kPpCurveAces) {
        const float s = max(c, 0.0) * F.acesContrast;
        const float m = (s * (2.51 * s + 0.03)) / (s * (2.43 * s + 0.59) + 0.14);
        return ppClamp(m / (1.0 + F.acesShoulder * m), 0.0, 1.0);
    }
    float mapped = ppFilmicSegment(c, F) * F.filmicWhiteScale;
    if (F.curveGamma != 1.0) {
        mapped = pow(max(mapped, 0.0), 1.0 / F.curveGamma);
    }
    return ppClamp(mapped, 0.0, 1.0);
}

// --- sRGB -------------------------------------------------------------------------------------------
float ppSrgbEncode(float c) {
    if (c <= 0.0031308) {
        return 12.92 * c;
    }
    return 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float ppSrgbDecode(float e) {
    if (e <= 0.04045) {
        return e / 12.92;
    }
    return pow((e + 0.055) / 1.055, 2.4);
}

// --- 3D LUT (tetrahedral) ---------------------------------------------------------------------------
float3 ppLutAt(float4* lut, uint r, uint g, uint b, uint n) { return lut[r + g * n + b * n * n].xyz; }
float3 ppLutTetrahedral(uint64_t address, uint n, float3 c) {
    float4* lut = ppTexels(address);
    const float scale = float(n - 1u);
    const float fr = ppSaturate(c.x) * scale;
    const float fg = ppSaturate(c.y) * scale;
    const float fb = ppSaturate(c.z) * scale;
    const uint r0 = min(uint(fr), n - 2u);
    const uint g0 = min(uint(fg), n - 2u);
    const uint b0 = min(uint(fb), n - 2u);
    const float dr = fr - float(r0);
    const float dg = fg - float(g0);
    const float db = fb - float(b0);
    const float3 c000 = ppLutAt(lut, r0, g0, b0, n);
    const float3 c111 = ppLutAt(lut, r0 + 1u, g0 + 1u, b0 + 1u, n);
    if (dr > dg) {
        if (dg > db) {
            const float3 c100 = ppLutAt(lut, r0 + 1u, g0, b0, n);
            const float3 c110 = ppLutAt(lut, r0 + 1u, g0 + 1u, b0, n);
            return ((c000 + (c100 - c000) * dr) + (c110 - c100) * dg) + (c111 - c110) * db;
        }
        if (dr > db) {
            const float3 c100 = ppLutAt(lut, r0 + 1u, g0, b0, n);
            const float3 c101 = ppLutAt(lut, r0 + 1u, g0, b0 + 1u, n);
            return ((c000 + (c100 - c000) * dr) + (c101 - c100) * db) + (c111 - c101) * dg;
        }
        const float3 c001 = ppLutAt(lut, r0, g0, b0 + 1u, n);
        const float3 c101 = ppLutAt(lut, r0 + 1u, g0, b0 + 1u, n);
        return ((c000 + (c001 - c000) * db) + (c101 - c001) * dr) + (c111 - c101) * dg;
    }
    if (db > dg) {
        const float3 c001 = ppLutAt(lut, r0, g0, b0 + 1u, n);
        const float3 c011 = ppLutAt(lut, r0, g0 + 1u, b0 + 1u, n);
        return ((c000 + (c001 - c000) * db) + (c011 - c001) * dg) + (c111 - c011) * dr;
    }
    if (db > dr) {
        const float3 c010 = ppLutAt(lut, r0, g0 + 1u, b0, n);
        const float3 c011 = ppLutAt(lut, r0, g0 + 1u, b0 + 1u, n);
        return ((c000 + (c010 - c000) * dg) + (c011 - c010) * db) + (c111 - c011) * dr;
    }
    const float3 c010 = ppLutAt(lut, r0, g0 + 1u, b0, n);
    const float3 c110 = ppLutAt(lut, r0 + 1u, g0 + 1u, b0, n);
    return ((c000 + (c010 - c000) * dg) + (c110 - c010) * dr) + (c111 - c110) * db;
}

// --- film grain -------------------------------------------------------------------------------------
uint64_t ppSplitmix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}
float ppGrainNoise(PpFrame F, uint px, uint py) {
    const uint64_t seed = (uint64_t(F.seedHi) << 32) | uint64_t(F.seedLo);
    const uint64_t pixel = (uint64_t(py) << 32) | uint64_t(px);
    const uint64_t hash = ppSplitmix64(ppSplitmix64(seed) ^ pixel);
    const float unit = (float(uint(hash >> 40)) + 0.5) * (1.0 / 16777216.0);
    return unit * 2.0 - 1.0;
}

// --- motion blur helpers ----------------------------------------------------------------------------
float ppDot2(float2 v) { return v.x * v.x + v.y * v.y; }
float ppLength2(float2 v) { return sqrt(ppDot2(v)); }
float2 ppHalfBlur(float2 velocity, PpFrame F) {
    float2 blur = velocity * F.mbShutter;
    const float len = ppLength2(blur);
    if (len > F.mbMaxBlur && len > 0.0) {
        blur = blur * (F.mbMaxBlur / len);
    }
    return blur * 0.5;
}
