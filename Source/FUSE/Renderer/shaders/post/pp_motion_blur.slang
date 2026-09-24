// WP-4.5 motion blur. Slang twin of pp_motion_blur.comp (see there).
#include "pp_common.slang"

float2 ppVelocity(PpFrame F, uint x, uint y) { return ppFetch(F.inputVelocity, int2(int(x), int(y))).xy; }
float ppDepth(PpFrame F, uint x, uint y) {
    return (F.flags & kPpFlagMbDepth) != 0u ? ppFetch(F.inputDepth, int2(int(x), int(y))).x : 0.0;
}
float ppCone(float distance, float halfLength) {
    if (halfLength <= 1e-6) {
        return 0.0;
    }
    return ppClamp(1.0 - distance / halfLength, 0.0, 1.0);
}
float ppSmoothstep(float e0, float e1, float x) {
    if (e1 <= e0) {
        return x < e0 ? 0.0 : 1.0;
    }
    const float t = ppClamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
float ppCylinder(float distance, float halfLength) {
    if (halfLength <= 1e-6) {
        return 0.0;
    }
    return 1.0 - ppSmoothstep(0.95 * halfLength, 1.05 * halfLength, distance);
}
float ppInFront(float a, float b, float extent) { return ppClamp(1.0 - (a - b) / max(extent, 1e-6), 0.0, 1.0); }

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const PpFrame F = ppFrame();
    const uint2 p = tid.xy;
    float2* tileMax = ppPairs(F.tileMax);
    float2* neighborMax = ppPairs(F.neighborMax);
    if (pc.mode == 0u) {
        if (p.x >= F.mbTilesX || p.y >= F.mbTilesY) {
            return;
        }
        float2 best = float2(0.0, 0.0);
        float bestLen = 0.0;
        const uint x0 = p.x * F.mbTile;
        const uint y0 = p.y * F.mbTile;
        const uint x1 = min(x0 + F.mbTile, F.width);
        const uint y1 = min(y0 + F.mbTile, F.height);
        for (uint y = y0; y < y1; ++y) {
            for (uint x = x0; x < x1; ++x) {
                const float2 h = ppHalfBlur(ppVelocity(F, x, y), F);
                const float len = ppDot2(h);
                if (len > bestLen) {
                    best = h;
                    bestLen = len;
                }
            }
        }
        tileMax[p.y * F.mbTilesX + p.x] = best;
        return;
    }
    if (pc.mode == 1u) {
        if (p.x >= F.mbTilesX || p.y >= F.mbTilesY) {
            return;
        }
        float2 best = float2(0.0, 0.0);
        float bestLen = 0.0;
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                const int nx = int(p.x) + ox;
                const int ny = int(p.y) + oy;
                if (nx < 0 || ny < 0 || nx >= int(F.mbTilesX) || ny >= int(F.mbTilesY)) {
                    continue;
                }
                const float2 c = tileMax[uint(ny) * F.mbTilesX + uint(nx)];
                const float len = ppDot2(c);
                if (len > bestLen) {
                    best = c;
                    bestLen = len;
                }
            }
        }
        neighborMax[p.y * F.mbTilesX + p.x] = best;
        return;
    }
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    float4* src = ppTexels(pc.src);
    const uint index = p.y * F.width + p.x;
    const float3 center = src[index].xyz;
    const float2 vn = neighborMax[(p.y / F.mbTile) * F.mbTilesX + p.x / F.mbTile];
    if (ppLength2(vn) < 0.5) {
        ppTexels(pc.dst)[index] = float4(center, 0.0);
        return;
    }
    const float2 vx = ppHalfBlur(ppVelocity(F, p.x, p.y), F);
    const float vxLength = ppLength2(vx);
    const float zx = ppDepth(F, p.x, p.y);
    float totalWeight = 1.0 / max(vxLength, 0.5);
    float3 sum = center * totalWeight;
    for (uint i = 0u; i < F.mbSamples; ++i) {
        const float t = -1.0 + (2.0 * (float(i) + 0.5)) / float(F.mbSamples);
        const int sx = ppLround(float(p.x) + vn.x * t);
        const int sy = ppLround(float(p.y) + vn.y * t);
        if (sx < 0 || sy < 0 || sx >= int(F.width) || sy >= int(F.height)) {
            continue;
        }
        if (sx == int(p.x) && sy == int(p.y)) {
            continue;
        }
        const uint sampleIndex = uint(sy) * F.width + uint(sx);
        const float dx = float(sx) - float(p.x);
        const float dy = float(sy) - float(p.y);
        const float distance = sqrt(dx * dx + dy * dy);
        const float vyLength = ppLength2(ppHalfBlur(ppVelocity(F, uint(sx), uint(sy)), F));
        const float zy = ppDepth(F, uint(sx), uint(sy));
        const float foreground = ppInFront(zy, zx, F.mbSoftDepth);
        const float background = ppInFront(zx, zy, F.mbSoftDepth);
        const float weight = (foreground * ppCone(distance, vyLength) + background * ppCone(distance, vxLength)) +
                             (ppCylinder(distance, vyLength) * ppCylinder(distance, vxLength)) * 2.0;
        sum = sum + src[sampleIndex].xyz * weight;
        totalWeight += weight;
    }
    ppTexels(pc.dst)[index] = float4(sum * (1.0 / totalWeight), 0.0);
}
