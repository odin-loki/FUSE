// WP-9.2 3D Gaussian splatting: Slang twin of gs_common.glsl (see there). Compile with -fp-mode precise so
// no multiply-add is contracted.
#pragma once

static const uint kGsTile = 16u;
static const uint kGsGroup = 256u;
static const uint kGsFlagDepthTest = 1u << 0;
static const uint kGsFlagReversedZ = 1u << 1;
static const uint kGsSplatFloats = 60u;
static const uint kGsPadValue = 0xFFFFFFFFu;

// GsFrameConstants, 224 bytes.
struct GsFrame {
    uint64_t splats;
    uint64_t projected;
    uint64_t offsets;
    uint64_t keys;
    uint64_t values;
    uint64_t ranges;
    uint64_t counters;
    uint64_t output_;
    uint splatCount;
    uint capacity;
    uint width;
    uint height;
    uint tilesX;
    uint tilesY;
    uint tileCount;
    uint depthHandle;
    uint flags;
    uint shDegree;
    uint reserved0;
    uint reserved1;
    float view[12];
    float camPos[4];
    float fx;
    float fy;
    float cx;
    float cy;
    float nearZ;
    float tanFovX;
    float tanFovY;
    float lowPass;
    float depthA;
    float depthB;
    float alphaMin;
    float transmittanceMin;
};

struct GsPush {
    uint64_t frame;
    uint padKeyHigh;
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<GsPush> pc;

// The bindless sampled images (binding 1 of the FUSE bindless set).
[[vk::binding(1, 0)]] Texture2D<float4> fuse_gs_textures[];

static GsFrame F;
void gsBegin() { F = ((GsFrame*)pc.frame)[0]; }

float* gsFloats(uint64_t address) { return (float*)address; }
uint4* gsProjectedPtr(uint64_t address) { return (uint4*)address; }
uint* gsUints(uint64_t address) { return (uint*)address; }
uint64_t* gsKeys(uint64_t address) { return (uint64_t*)address; }
float4* gsTexels(uint64_t address) { return (float4*)address; }

float gsClamp(float v, float lo, float hi) { return v < lo ? lo : (hi < v ? hi : v); }
uint4 gsProjected(uint i, uint w) { return gsProjectedPtr(F.projected)[i * 4u + w]; }
uint gsTilesTouched(uint4 rect) { return (rect.z - rect.x) * (rect.w - rect.y); }

// Correctly rounded division and square root. The CPU reference (gsplat_reference.cpp) divides and takes square
// roots in IEEE f32; Vulkan only guarantees correct rounding for add / sub / mul (OpFDiv is 2.5 ulp, sqrt inherits
// inversesqrt), and hardware uses that slack (the RTX 3090 differs from the oracle by up to ~250 ulp on the conic,
// where det = a c - b^2 amplifies the Jacobian's division error, and the 3-sigma radius ceil(3 sqrt(lambda)) and the
// tile rectangle floor((mx -+ r) / 16) are discrete decisions on these quotients). gs_div / gs_sqrt round the
// hardware result to the IEEE one with an exact Dekker residual (needs only correctly rounded, uncontracted mul /
// add), so "gsplat.preprocess" equals gs_preprocess_splat bit for bit on any conformant device. Lavapipe already
// rounds correctly: there the walk never moves.
float gsResid(float a, float q, float b) { // a - q * b, accurate enough to compare neighbours
    const float p = q * b;
    const float cq = 4097.0 * q;
    const float qh = cq - (cq - q);
    const float ql = q - qh;
    const float cb = 4097.0 * b;
    const float bh = cb - (cb - b);
    const float bl = b - bh;
    const float e = ((qh * bh - p) + qh * bl + ql * bh) + ql * bl;
    return (a - p) - e;
}
float gsNextToward(float q, bool up) { // q != 0; up: toward +inf
    const uint bits = asuint(q);
    return asfloat((q > 0.0) == up ? bits + 1u : bits - 1u);
}
float gsRoundRn(float a, float q, float b, bool root) { // walk q to the nearest representable a / b (root: sqrt(a))
    float r = gsResid(a, q, root ? q : b);
    for (int i = 0; i < 4 && r != 0.0; ++i) {
        const float qn = gsNextToward(q, root ? r > 0.0 : (r > 0.0) == (b > 0.0));
        const float rn = gsResid(a, qn, root ? qn : b);
        if (abs(rn) < abs(r) || (abs(rn) == abs(r) && (asuint(q) & 1u) != 0u)) {
            q = qn;
            r = rn;
        } else {
            break;
        }
    }
    return q;
}
float gsDiv(float a, float b) {
    const float q = a / b;
    if (q == 0.0 || isinf(q) || isnan(q)) return q;
    return gsRoundRn(a, q, b, false);
}
float gsSqrt(float x) {
    const float s = sqrt(x);
    if (!(x > 0.0) || isinf(x)) return s;
    return gsRoundRn(x, s, s, true);
}
