// WP-6.4 SVGF / A-SVGF: Slang twin of dn_common.glsl (see there). Compile with -fp-mode precise (Slang does
// not honour `precise` per variable), so no multiply-add is contracted.
#pragma once

static const uint kDnFlagHistory = 1u << 0;
static const uint kDnFlagGradients = 1u << 1;
static const uint kDnFlagScalar = 1u << 2;
static const uint kDnFlagNormalOct = 1u << 3;
static const uint kDnFlagNormalImage = 1u << 4;
static const uint kDnFlagSpatialVariance = 1u << 5;

static const uint kDnPassWriteImage = 1u << 0;

static const uint kDnGradientPrepare = 0u;
static const uint kDnGradientAtrous = 1u;

static const uint kDnStratum = 3u;

// DenoiseFrameConstants, 240 bytes.
struct DnFrame {
    uint64_t signal;
    uint64_t motion;
    uint64_t depth;
    uint64_t normal;
    uint64_t gradientIn;
    uint64_t guideCur;
    uint64_t guidePrev;
    uint64_t gradZ;
    uint64_t histPrev;
    uint64_t histCur;
    uint64_t momPrev;
    uint64_t momCur;
    uint64_t accum;
    uint64_t lambda;
    uint64_t output_;
    uint64_t reserved0;
    uint width;
    uint height;
    uint strataW;
    uint strataH;
    uint normalImage;
    uint outputImage;
    uint flags;
    uint signalStride;
    float alphaColor;
    float alphaMoments;
    float maxHistory;
    float reprojDepth;
    float reprojNormal;
    float minReprojWeight;
    float varianceHistory;
    float varianceBoost;
    float sigmaDepth;
    float sigmaLuminance;
    float sigmaVarianceLum;
    float depthEpsilon;
    uint sigmaNormal;
    uint atrousIterations;
    uint historyTap;
    uint gradientIterations;
    float gradientScale;
    float gradientEpsilon;
    uint reserved1;
    uint reserved2;
};

struct DnPush {
    uint64_t frame;
    uint64_t src;
    uint64_t dst;
    uint64_t aux;
    uint mode;
    uint step;
    uint flags;
    uint reserved0;
    uint reserved1;
    uint reserved2;
    uint reserved3;
    uint reserved4;
};
[[vk::push_constant]] ConstantBuffer<DnPush> pc;

[[vk::binding(1, 0)]] Texture2D<float4> fuse_dn_textures[];

DnFrame dnFrame() { return ((DnFrame*)pc.frame)[0]; }
float4* dnTexels(uint64_t address) { return (float4*)address; }
float2* dnPairs(uint64_t address) { return (float2*)address; }
float* dnFloats(uint64_t address) { return (float*)address; }

float dnAtrousTap(int d) {
    const int a = d < 0 ? -d : d;
    return a == 0 ? 0.375 : (a == 1 ? 0.25 : 0.0625);
}
float dnGaussTap(int d) { return d == 0 ? 0.5 : 0.25; }

float dnLuminance(float4 c, uint flags) {
    if ((flags & kDnFlagScalar) != 0u) {
        return c.x;
    }
    return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
}

float dnPowInt(float b, uint e) {
    float r = 1.0;
    float bb = b;
    uint k = e;
    while (k != 0u) {
        if ((k & 1u) != 0u) {
            r = r * bb;
        }
        bb = bb * bb;
        k = k >> 1u;
    }
    return r;
}

float dnDot3(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

float dnNormalWeight(float4 p, float4 q, uint sigmaNormal) {
    const float d = dnDot3(p, q);
    return dnPowInt(d > 0.0 ? d : 0.0, sigmaNormal);
}

float dnDepthTerm(DnFrame F, float zp, float zq, float2 g, float ox, float oy) {
    const float proj = g.x * ox + g.y * oy;
    return abs(zp - zq) / (F.sigmaDepth * abs(proj) + F.depthEpsilon);
}

float4 dnOctDecode(float ox, float oy) {
    float nx = ox;
    float ny = oy;
    const float nz = 1.0 - abs(ox) - abs(oy);
    if (nz < 0.0) {
        nx = (1.0 - abs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
        ny = (1.0 - abs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
    }
    const float len = sqrt(nx * nx + ny * ny + nz * nz);
    return float4(nx / len, ny / len, nz / len, 0.0);
}

float4 dnReadSignal(DnFrame F, uint i) {
    float* s = dnFloats(F.signal);
    if (F.signalStride == 1u) {
        return float4(s[i], 0.0, 0.0, 0.0);
    }
    const uint b = i * F.signalStride;
    return float4(s[b], s[b + 1u], s[b + 2u], 0.0);
}

float dnGradientLambda(DnFrame F, float4 g) {
    if (!(g.y > F.gradientEpsilon)) {
        return 0.0;
    }
    const float l = F.gradientScale * abs(g.x) / g.y;
    return l < 1.0 ? l : 1.0;
}

uint dnStratumPixel(DnFrame F, uint sx, uint sy) {
    const uint px = sx * kDnStratum + 1u < F.width ? sx * kDnStratum + 1u : F.width - 1u;
    const uint py = sy * kDnStratum + 1u < F.height ? sy * kDnStratum + 1u : F.height - 1u;
    return py * F.width + px;
}
