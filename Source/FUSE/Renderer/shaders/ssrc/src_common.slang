// Screen-space radiance cascades: Slang twin of src_common.glsl (see there). Compile with -fp-mode precise (Slang
// does not honour `precise` per variable), so no multiply-add is contracted.
#pragma once

static const uint kSrcFlagBilinearFix = 1u << 0;
static const uint kSrcFlagReversedZ = 1u << 1;
static const uint kSrcFlagDdgi = 1u << 2;
static const uint kSrcFlagCompose = 1u << 3;

// SsrcFrameConstants, 608 bytes.
struct SrcFrame {
    uint64_t geo;
    uint64_t lit;
    uint64_t diffuse;
    uint64_t albedo;
    uint64_t indirect;
    uint64_t records0;
    uint64_t records1;
    uint64_t dirs;
    uint64_t ddgiVolume;
    uint64_t dump;
    uint64_t reserved0;
    uint64_t reserved1;
    uint width;
    uint height;
    uint inputDepth;
    uint inputNormal;
    uint inputAlbedo;
    uint inputRoughMetal;
    uint inputLit;
    uint output_;
    uint flags;
    uint cascades;
    uint aoCascades;
    uint reserved2;
    float fx;
    float fy;
    float cx;
    float cy;
    float nearZ;
    float nearPlane;
    float farPlane;
    float reserved3;
    float viewRot[12];
    float viewToWorld[12];
    float cameraWorld[4];
    float ambient[4];
    float sky[4];
    float stride;
    float thickness;
    float thicknessSlope;
    float maxDistance;
    float originBias;
    float intensity;
    float ddgiScale;
    float planeTolerance;
    uint probesX[8];
    uint probesY[8];
    uint dirRes[8];
    uint dirOffset[8];
    float spacing[8];
    float tStart[8];
    float tEnd[8];
    float reserved5[8];
};

// SsrcPush, 40 bytes (scalars and uint64_t only: identical under std140 and std430).
struct SrcPush {
    uint64_t frame;
    uint64_t upper;
    uint64_t dst;
    uint cascade;
    uint count;
    uint groupsX;
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<SrcPush> pc;

// The frame constants, loaded once per invocation by srcBegin().
static SrcFrame F;

void srcBegin() { F = ((SrcFrame*)pc.frame)[0]; }

float4* srcTexels(uint64_t address) { return (float4*)address; }
uint2* srcRecords(uint64_t address) { return (uint2*)address; }

// --- f16 records ---------------------------------------------------------------------------------------------
uint srcF32ToF16(float v) {
    float m = v;
    if (m > 65504.0) {
        m = 65504.0;
    }
    if (m < -65504.0) {
        m = -65504.0;
    }
    uint u = asuint(m);
    const uint sign = u & 0x80000000u;
    u ^= sign;
    uint o = 0u;
    if (u >= 0x47800000u) {
        o = u > 0x7F800000u ? 0x7E00u : 0x7C00u;
    } else if (u < 0x38800000u) {
        const float t = asfloat(u) + 0.5;
        o = asuint(t) - 0x3F000000u;
    } else {
        const uint odd = (u >> 13) & 1u;
        u += 0xC8000FFFu;
        u += odd;
        o = u >> 13;
    }
    return o | (sign >> 16);
}

float srcF16ToF32(uint h) {
    const uint sign = (h & 0x8000u) << 16;
    const uint e = (h >> 10) & 0x1Fu;
    const uint m = h & 0x3FFu;
    if (e == 0u) {
        const float f = float(m) * 5.9604644775390625e-8;
        return sign != 0u ? -f : f;
    }
    if (e == 31u) {
        return asfloat(sign | 0x7F800000u | (m << 13));
    }
    return asfloat(sign | ((e + 112u) << 23) | (m << 13));
}

float4 srcDecode(uint2 r) {
    return float4(srcF16ToF32(r.x & 0xFFFFu), srcF16ToF32(r.x >> 16), srcF16ToF32(r.y & 0xFFFFu),
                  srcF16ToF32(r.y >> 16));
}

// --- vector helpers -----------------------------------------------------------------------------------------
float srcDot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float3 srcScale(float3 v, float s) { return float3(v.x * s, v.y * s, v.z * s); }
float3 srcAdd(float3 a, float3 b) { return float3(a.x + b.x, a.y + b.y, a.z + b.z); }
float3 srcSub(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
float3 srcNormalize(float3 v) {
    const float len = sqrt(srcDot(v, v));
    if (len < 1e-8) {
        return float3(0.0);
    }
    return srcScale(v, 1.0 / len);
}
float srcRayCoord(uint p, float c, float f) { return ((float(p) + 0.5) - c) / f; }

uint srcClampIndex(int v, uint count) {
    if (v < 0) {
        return 0u;
    }
    const uint u = uint(v);
    return u >= count ? count - 1u : u;
}

uint4 srcBilinear(float px, float py, float spacing, uint countX, uint countY, out float4 w) {
    const float u = px / spacing - 0.5;
    const float v = py / spacing - 0.5;
    const float fu = floor(u);
    const float fv = floor(v);
    const float ax = u - fu;
    const float ay = v - fv;
    const int ix = int(fu);
    const int iy = int(fv);
    const float bx = 1.0 - ax;
    const float by = 1.0 - ay;
    w = float4(bx * by, ax * by, bx * ay, ax * ay);
    return uint4(srcClampIndex(ix, countX), srcClampIndex(ix + 1, countX), srcClampIndex(iy, countY),
                 srcClampIndex(iy + 1, countY));
}

// --- probes ---------------------------------------------------------------------------------------------------
struct SrcProbe {
    bool valid;
    uint pixel;
    float3 o;
    float3 n; // camera-facing unit normal of the probe pixel
};

float srcGrid(uint p, float s) { return (float(p) + 0.5) * s; }

SrcProbe srcProbeAt(uint ci, uint probeX, uint probeY) {
    SrcProbe p;
    p.valid = false;
    p.o = float3(0.0);
    p.n = float3(0.0);
    const float s = F.spacing[ci];
    const uint ux = min(uint(srcGrid(probeX, s)), F.width - 1u);
    const uint uy = min(uint(srcGrid(probeY, s)), F.height - 1u);
    p.pixel = uy * F.width + ux;
    const float4 g = srcTexels(F.geo)[p.pixel];
    const float z = g.x;
    if (!(z > 0.0)) {
        return p;
    }
    const float a = srcRayCoord(ux, F.cx, F.fx);
    const float b = srcRayCoord(uy, F.cy, F.fy);
    float3 n = srcNormalize(g.yzw);
    if (srcDot(n, float3(a, b, 1.0)) > 0.0) {
        n = srcScale(n, -1.0);
    }
    p.o = srcAdd(float3(a * z, b * z, z), srcScale(n, F.originBias * z));
    p.n = n;
    p.valid = true;
    return p;
}

// Gather weight of cascade 0 direction k (binWeight).
float srcBinWeight(uint k, float3 n) {
    if (F.cascades < 2u) {
        const float4 dt = srcTexels(F.dirs)[k];
        return max(0.0, srcDot(n, dt.xyz)) * dt.w;
    }
    const uint res = F.dirRes[0];
    const uint res2 = F.dirRes[1];
    const uint ku = k % res;
    const uint kv = k / res;
    float w = 0.0;
    for (uint j = 0u; j < 4u; ++j) {
        const uint ck = (2u * kv + (j >> 1u)) * res2 + 2u * ku + (j & 1u);
        const float4 dt = srcTexels(F.dirs)[F.dirOffset[1] + ck];
        w = w + max(0.0, srcDot(n, dt.xyz)) * dt.w;
    }
    return w;
}
