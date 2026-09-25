// WP-6.5 radiance cascades (research): Slang twin of rc_common.glsl (see there). Compile with -fp-mode precise
// (Slang does not honour `precise` per variable), so no multiply-add is contracted.
#pragma once

static const uint kRcFlagBilinearFix = 1u << 0;
static const uint kRcFlagHasUpper = 1u << 1;

// RcPush (include/fuse/renderer/research/rc/rc_types.hpp), 112 bytes (no arrays: push constants are laid out
// std140 by default, and scalars / uint64_t place identically under std140 and std430).
struct RcPush {
    uint64_t scene;
    uint64_t dirs;
    uint64_t upper;
    uint64_t dst;
    uint width;
    uint height;
    uint probesX;
    uint probesY;
    uint dirCount;
    uint upperProbesX;
    uint upperProbesY;
    uint flags;
    float spacing;
    float upperSpacing;
    float tStart;
    float tEnd;
    float step;
    float invStep;
    float skyR;
    float skyG;
    float skyB;
    uint count;
    uint branch;
    float invBranch;
};
[[vk::push_constant]] ConstantBuffer<RcPush> pc;

float4* rcTexels(uint64_t address) { return (float4*)address; }

float4 rcTrace(float2 a, float2 d, float len) {
    const float n = ceil(len * pc.invStep);
    const uint count = n > 0.0 ? uint(n) : 0u;
    const float fw = float(pc.width);
    const float fh = float(pc.height);
    for (uint k = 0u; k < count; ++k) {
        const float t = float(k) * pc.step;
        const float px = a.x + d.x * t;
        const float py = a.y + d.y * t;
        const float fx = floor(px);
        const float fy = floor(py);
        if (fx < 0.0 || fy < 0.0 || fx >= fw || fy >= fh) {
            if ((fx < 0.0 && d.x <= 0.0) || (fx >= fw && d.x >= 0.0) || (fy < 0.0 && d.y <= 0.0) ||
                (fy >= fh && d.y >= 0.0)) {
                break;
            }
            continue;
        }
        const float4 v = rcTexels(pc.scene)[uint(fy) * pc.width + uint(fx)];
        if (v.w > 0.5) {
            return float4(v.xyz, 0.0);
        }
    }
    return float4(0.0, 0.0, 0.0, 1.0);
}

uint rcClampIndex(int v, uint count) {
    if (v < 0) {
        return 0u;
    }
    const uint u = uint(v);
    return u >= count ? count - 1u : u;
}

uint4 rcBilinear(float2 p, float spacing, uint countX, uint countY, out float4 w) {
    const float u = p.x / spacing - 0.5;
    const float v = p.y / spacing - 0.5;
    const float fu = floor(u);
    const float fv = floor(v);
    const float ax = u - fu;
    const float ay = v - fv;
    const int ix = int(fu);
    const int iy = int(fv);
    const float bx = 1.0 - ax;
    const float by = 1.0 - ay;
    w = float4(bx * by, ax * by, bx * ay, ax * ay);
    return uint4(rcClampIndex(ix, countX), rcClampIndex(ix + 1, countX), rcClampIndex(iy, countY),
                 rcClampIndex(iy + 1, countY));
}

float3 rcChildAverage(uint x, uint y, uint k) {
    const uint upperDirs = pc.dirCount * pc.branch;
    const uint base = (y * pc.upperProbesX + x) * upperDirs + k * pc.branch;
    float r = 0.0;
    float g = 0.0;
    float b = 0.0;
    for (uint j = 0u; j < pc.branch; ++j) {
        const float4 v = rcTexels(pc.upper)[base + j];
        r = r + v.x;
        g = g + v.y;
        b = b + v.z;
    }
    return float3(r * pc.invBranch, g * pc.invBranch, b * pc.invBranch);
}
