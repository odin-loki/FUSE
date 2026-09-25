// WP-6.5 "rc.cascade". Slang twin of rc_cascade.comp (see there).
#include "rc_common.slang"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint index = tid.x;
    if (index >= pc.count) {
        return;
    }
    const uint k = index % pc.dirCount;
    const uint probe = index / pc.dirCount;
    const uint probeX = probe % pc.probesX;
    const uint probeY = probe / pc.probesX;
    const float px = (float(probeX) + 0.5) * pc.spacing;
    const float py = (float(probeY) + 0.5) * pc.spacing;
    const float4 dir = rcTexels(pc.dirs)[k];
    const float dx = dir.x;
    const float dy = dir.y;
    const float ax = px + dx * pc.tStart;
    const float ay = py + dy * pc.tStart;
    float r = 0.0;
    float g = 0.0;
    float b = 0.0;
    if ((pc.flags & kRcFlagHasUpper) == 0u) {
        const float4 h = rcTrace(float2(ax, ay), float2(dx, dy), pc.tEnd - pc.tStart);
        r = h.x + h.w * pc.skyR;
        g = h.y + h.w * pc.skyG;
        b = h.z + h.w * pc.skyB;
    } else {
        float4 w;
        const uint4 q = rcBilinear(float2(px, py), pc.upperSpacing, pc.upperProbesX, pc.upperProbesY, w);
        if ((pc.flags & kRcFlagBilinearFix) == 0u) {
            const float4 h = rcTrace(float2(ax, ay), float2(dx, dy), pc.tEnd - pc.tStart);
            float fr = 0.0;
            float fg = 0.0;
            float fb = 0.0;
            for (uint j = 0u; j < 4u; ++j) {
                const float3 c = rcChildAverage((j & 1u) != 0u ? q.y : q.x, (j >> 1u) != 0u ? q.w : q.z, k);
                fr = fr + w[j] * c.x;
                fg = fg + w[j] * c.y;
                fb = fb + w[j] * c.z;
            }
            r = h.x + h.w * fr;
            g = h.y + h.w * fg;
            b = h.z + h.w * fb;
        } else {
            for (uint j = 0u; j < 4u; ++j) {
                const uint ux = (j & 1u) != 0u ? q.y : q.x;
                const uint uy = (j >> 1u) != 0u ? q.w : q.z;
                const float qx = (float(ux) + 0.5) * pc.upperSpacing;
                const float qy = (float(uy) + 0.5) * pc.upperSpacing;
                const float bx = qx + dx * pc.tEnd;
                const float by = qy + dy * pc.tEnd;
                const float sx = bx - ax;
                const float sy = by - ay;
                const float l2 = sx * sx + sy * sy;
                float4 h = float4(0.0, 0.0, 0.0, 1.0);
                if (l2 > 1e-12) {
                    const float len = sqrt(l2);
                    const float inv = 1.0 / len;
                    h = rcTrace(float2(ax, ay), float2(sx * inv, sy * inv), len);
                }
                const float3 c = rcChildAverage(ux, uy, k);
                r = r + w[j] * (h.x + h.w * c.x);
                g = g + w[j] * (h.y + h.w * c.y);
                b = b + w[j] * (h.z + h.w * c.z);
            }
        }
    }
    rcTexels(pc.dst)[index] = float4(r, g, b, 0.0);
}
