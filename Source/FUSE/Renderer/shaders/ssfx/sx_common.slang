// WP-6.3 screen-space fallback on Vulkan: Slang twin of sx_common.glsl (see there). Compile with
// -fp-mode precise (Slang does not honour `precise` per variable), so no multiply-add is contracted.
#pragma once

static const uint kSxFlagAo = 1u << 0;
static const uint kSxFlagSsr = 1u << 1;
static const uint kSxFlagSsgi = 1u << 2;
static const uint kSxFlagReversedZ = 1u << 3;
static const uint kSxFlagMultiBounce = 1u << 4;
static const uint kSxFlagSsrRoughness = 1u << 5;
static const uint kSxFlagSsrContact = 1u << 6;
static const uint kSxFlagAoJitter = 1u << 7;

// SsfxFrameConstants, 592 bytes.
struct SxFrame {
    uint64_t prepared;
    uint64_t normals;
    uint64_t radiance;
    uint64_t albedo;
    uint64_t diffuse;
    uint64_t ao;
    uint64_t ssr;
    uint64_t gi;
    uint64_t bounce0;
    uint64_t bounce1;
    uint64_t dump;
    uint64_t reserved0;
    uint width;
    uint height;
    uint inputDepth;
    uint inputNormal;
    uint inputAlbedo;
    uint inputRoughMetal;
    uint inputLit;
    uint output_;
    uint flags;
    uint reserved1;
    float fx;
    float fy;
    float cx;
    float cy;
    float nearZ;
    float nearPlane;
    float farPlane;
    float reserved2;
    float viewRot[12];
    float ambient[4];
    float aoRadius;
    float aoFalloff;
    float aoBias;
    float aoStrength;
    float aoMaxRadiusPx;
    uint aoSlices;
    uint aoSteps;
    uint aoFrame;
    float aoSliceCos[32];
    float aoSliceSin[32];
    uint ssrMaxSteps;
    uint ssrRefineSteps;
    float ssrStride;
    float ssrThickness;
    float ssrMaxDistance;
    float ssrFadeEdge;
    float ssrContactDistance;
    float ssrContactFloor;
    float ssrContactExponent;
    uint reserved3;
    uint giSampleSqrt;
    uint giBounces;
    uint giMaxSteps;
    uint giRefineSteps;
    float giStride;
    float giThickness;
    float giMaxDistance;
    float giIntensity;
};

struct SxPush {
    uint64_t frame;
    uint64_t src;
    uint64_t dst;
    uint mode;
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<SxPush> pc;

[[vk::binding(1, 0)]] Texture2D<float4> fuse_sx_textures[];

// The frame constants, loaded once per invocation by sxBegin().
static SxFrame F;

void sxBegin() { F = ((SxFrame*)pc.frame)[0]; }

float4* sxTexels(uint64_t address) { return (float4*)address; }
float* sxFloats(uint64_t address) { return (float*)address; }

float4 sxFetch(uint handle, int2 p) { return fuse_sx_textures[handle & 0xFFFFFu].Load(int3(p, 0)); }

// --- scalar / vector helpers ----------------------------------------------------------------------
float sxSaturate(float v) { return max(0.0, min(1.0, v)); }
float sxClamp(float v, float lo, float hi) { return v < lo ? lo : (hi < v ? hi : v); }
float sxDot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float sxLength(float3 v) { return sqrt(sxDot(v, v)); }
float3 sxScale(float3 v, float s) { return float3(v.x * s, v.y * s, v.z * s); }
float3 sxAdd(float3 a, float3 b) { return float3(a.x + b.x, a.y + b.y, a.z + b.z); }
float3 sxSub(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
float3 sxNormalize(float3 v) {
    const float len = sxLength(v);
    if (len < 1e-8) {
        return float3(0.0);
    }
    return sxScale(v, 1.0 / len);
}
float3 sxCross(float3 a, float3 b) {
    return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
float3 sxNeg(float3 v) { return sxScale(v, -1.0); }

// --- SsfxCamera / SsfxGBufferView over the prepared sections --------------------------------------
uint sxIndex(uint x, uint y) { return y * F.width + x; }
float sxDepthAt(uint x, uint y) { return sxTexels(F.prepared)[sxIndex(x, y)].x; }
float3 sxNormalAt(uint x, uint y) { return sxTexels(F.normals)[sxIndex(x, y)].xyz; }
float3 sxColorAt(uint64_t address, uint i) { return sxTexels(address)[i].xyz; }

float3 sxRayDirection(float px, float py) { return float3((px - F.cx) / F.fx, (py - F.cy) / F.fy, 1.0); }
float3 sxUnproject(float px, float py, float viewZ) { return sxScale(sxRayDirection(px, py), viewZ); }
float3 sxPositionAt(uint x, uint y) { return sxUnproject(float(x) + 0.5, float(y) + 0.5, sxDepthAt(x, y)); }
bool sxProject(float3 p, out float px, out float py) {
    px = 0.0;
    py = 0.0;
    if (p.z < F.nearZ) {
        return false;
    }
    px = F.fx * p.x / p.z + F.cx;
    py = F.fy * p.y / p.z + F.cy;
    return true;
}
bool sxInside(float px, float py) { return px >= 0.0 && py >= 0.0 && px < float(F.width) && py < float(F.height); }

float sxHashUnit(uint v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return float(v >> 8) * (1.0 / 16777216.0);
}

void sxTangentBasis(float3 n, out float3 t, out float3 b) {
    const float3 helper = abs(n.x) < 0.9 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    t = sxNormalize(sxCross(helper, n));
    b = sxCross(n, t);
}
