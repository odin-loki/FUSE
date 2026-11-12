// "ssfx.hiz". Slang twin of sx_hiz.comp (see there) and of ssfx_gpu::hiz_texel.
#include "sx_common.slang"

uint sxHizExtent(uint extent, uint level) { return (extent + (1u << level) - 1u) >> level; }

float sxHizBelow(uint level, uint x, uint y) {
    if (level == 0u) {
        const float z = sxDepthAt(x, y);
        return z > 0.0 ? z : kSxHizSky;
    }
    return sxFloats(F.hiz)[F.hizOffset[level] + y * sxHizExtent(F.width, level) + x];
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    sxBegin();
    const uint level = pc.mode;
    const uint2 p = tid.xy;
    const uint w = sxHizExtent(F.width, level);
    const uint h = sxHizExtent(F.height, level);
    if (level == 0u || level > F.hizLevels || p.x >= w || p.y >= h) {
        return;
    }
    const uint bw = sxHizExtent(F.width, level - 1u);
    const uint bh = sxHizExtent(F.height, level - 1u);
    const uint x0 = p.x * 2u;
    const uint y0 = p.y * 2u;
    const uint x1 = x0 + 1u < bw ? x0 + 1u : x0;
    const uint y1 = y0 + 1u < bh ? y0 + 1u : y0;
    const float a = sxHizBelow(level - 1u, x0, y0);
    const float b = sxHizBelow(level - 1u, x1, y0);
    const float c = sxHizBelow(level - 1u, x0, y1);
    const float d = sxHizBelow(level - 1u, x1, y1);
    const float ab = a < b ? a : b;
    const float cd = c < d ? c : d;
    sxFloats(F.hiz)[F.hizOffset[level] + p.y * w + p.x] = ab < cd ? ab : cd;
}
