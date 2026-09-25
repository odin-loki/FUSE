// WP-6.5 "rc.gather". Slang twin of rc_gather.comp (see there).
#include "rc_common.slang"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint index = tid.x;
    if (index >= pc.count) {
        return;
    }
    const uint x = index % pc.width;
    const uint y = index / pc.width;
    float4 w;
    const uint4 q = rcBilinear(float2(float(x) + 0.5, float(y) + 0.5), pc.spacing, pc.probesX, pc.probesY, w);
    const float inv = 1.0 / float(pc.dirCount);
    float r = 0.0;
    float g = 0.0;
    float b = 0.0;
    for (uint j = 0u; j < 4u; ++j) {
        const uint base = (((j >> 1u) != 0u ? q.w : q.z) * pc.probesX + ((j & 1u) != 0u ? q.y : q.x)) * pc.dirCount;
        float sr = 0.0;
        float sg = 0.0;
        float sb = 0.0;
        for (uint k = 0u; k < pc.dirCount; ++k) {
            const float4 v = rcTexels(pc.upper)[base + k];
            sr = sr + v.x;
            sg = sg + v.y;
            sb = sb + v.z;
        }
        r = r + w[j] * (sr * inv);
        g = g + w[j] * (sg * inv);
        b = b + w[j] * (sb * inv);
    }
    rcTexels(pc.dst)[index] = float4(r, g, b, 1.0);
}
