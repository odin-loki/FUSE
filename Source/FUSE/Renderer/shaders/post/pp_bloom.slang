// WP-4.5 bloom. Slang twin of pp_bloom.comp (see there).
#include "pp_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const PpFrame F = ppFrame();
    const uint2 p = tid.xy;
    if (p.x >= pc.dstW || p.y >= pc.dstH) {
        return;
    }
    float4* src = ppTexels(pc.src);
    float4* dst = ppTexels(pc.dst);
    const uint o = p.y * pc.dstW + p.x;
    if (pc.mode == 0u) {
        const float3 hdr = src[o].xyz;
        const float lum = ppLuminance(hdr);
        const float knee = max(F.bloomKnee, 0.0);
        float response = 0.0;
        if (lum <= F.bloomThreshold - knee) {
            response = 0.0;
        } else if (knee > 0.0 && lum < F.bloomThreshold + knee) {
            const float soft = lum - F.bloomThreshold + knee;
            response = (soft * soft) / (4.0 * knee);
        } else {
            response = max(lum - F.bloomThreshold, 0.0);
        }
        const float3 bright = (response <= 0.0 || lum <= 0.0) ? float3(0.0, 0.0, 0.0) : hdr * (response / lum);
        dst[o] = float4(bright, 0.0);
        return;
    }
    if (pc.mode == 1u || pc.mode == 2u) {
        const float w[4] = { 1.0 / 8.0, 3.0 / 8.0, 3.0 / 8.0, 1.0 / 8.0 };
        float3 sum = float3(0.0, 0.0, 0.0);
        for (int k = 0; k < 4; ++k) {
            float3 s;
            if (pc.mode == 1u) {
                s = src[p.y * pc.srcW + ppClampIndex(2 * int(p.x) - 1 + k, pc.srcW)].xyz;
            } else {
                s = src[ppClampIndex(2 * int(p.y) - 1 + k, pc.srcH) * pc.srcW + p.x].xyz;
            }
            sum = sum + s * w[k];
        }
        dst[o] = float4(sum, 0.0);
        return;
    }
    if (pc.mode == 3u || pc.mode == 4u) {
        const bool horizontal = pc.mode == 3u;
        const uint pos = horizontal ? p.x : p.y;
        const uint srcSize = horizontal ? pc.srcW : pc.srcH;
        const int i = int(pos / 2u);
        const int other = (pos % 2u == 0u) ? i - 1 : i + 1;
        const uint a = ppClampIndex(i, srcSize);
        const uint b = ppClampIndex(other, srcSize);
        const float3 va = horizontal ? src[p.y * pc.srcW + a].xyz : src[a * pc.srcW + p.x].xyz;
        const float3 vb = horizontal ? src[p.y * pc.srcW + b].xyz : src[b * pc.srcW + p.x].xyz;
        float3 up = va * 0.75 + vb * 0.25;
        if (pc.aux != 0ULL) {
            const float3 down = ppTexels(pc.aux)[o].xyz;
            const float scatter = ppClamp(F.bloomScatter, 0.0, 1.0);
            up = down * (1.0 - scatter) + up * scatter;
        }
        dst[o] = float4(up, 0.0);
        return;
    }
    const float3 bloom = ppTexels(pc.aux)[o].xyz;
    const float3 b = bloom * F.bloomIntensity;
    const float3 tinted = float3(b.x * F.bloomTint[0], b.y * F.bloomTint[1], b.z * F.bloomTint[2]);
    dst[o] = float4(dst[o].xyz + tinted, 0.0);
}
