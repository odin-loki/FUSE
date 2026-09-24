// WP-6.4 "denoise.variance". Slang twin of dn_variance.comp (see there).
#include "dn_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const DnFrame F = dnFrame();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const uint w = F.width;
    const uint i = p.y * w + p.x;
    float4* guide = dnTexels(F.guideCur);
    float4* accum = dnTexels(F.accum);
    float4* moments = dnTexels(F.momCur);
    float4* dst = dnTexels(pc.dst);
    const float4 g = guide[i];
    const float4 a = accum[i];
    const float4 mo = moments[i];
    if (!(g.w > 0.0)) {
        dst[i] = float4(a.xyz, 0.0);
        return;
    }
    const float len = mo.z;
    if ((F.flags & kDnFlagSpatialVariance) == 0u || !(len < F.varianceHistory)) {
        const float v = mo.y - mo.x * mo.x;
        dst[i] = float4(a.xyz, v > 0.0 ? v : 0.0);
        return;
    }
    const float2 gz = dnPairs(F.gradZ)[i];
    const float lp = dnLuminance(a, F.flags);
    float sumW = 0.0;
    float sr = 0.0;
    float sg = 0.0;
    float sb = 0.0;
    float s1 = 0.0;
    float s2 = 0.0;
    for (int dy = -3; dy <= 3; ++dy) {
        for (int dx = -3; dx <= 3; ++dx) {
            const int qx = int(p.x) + dx;
            const int qy = int(p.y) + dy;
            if (qx < 0 || qy < 0 || qx >= int(w) || qy >= int(F.height)) {
                continue;
            }
            const uint q = uint(qy) * w + uint(qx);
            const float4 gq = guide[q];
            if (!(gq.w > 0.0)) {
                continue;
            }
            const float4 aq = accum[q];
            const float4 mq = moments[q];
            const float wz = dnDepthTerm(F, g.w, gq.w, gz, float(dx), float(dy));
            const float wn = dnNormalWeight(g, gq, F.sigmaNormal);
            const float wl = abs(lp - dnLuminance(aq, F.flags)) / F.sigmaVarianceLum;
            const float wgt = wn * exp(-(wz + wl));
            sumW = sumW + wgt;
            sr = sr + wgt * aq.x;
            sg = sg + wgt * aq.y;
            sb = sb + wgt * aq.z;
            s1 = s1 + wgt * mq.x;
            s2 = s2 + wgt * mq.y;
        }
    }
    if (!(sumW > 0.0)) {
        const float v = mo.y - mo.x * mo.x;
        dst[i] = float4(a.xyz, v > 0.0 ? v : 0.0);
        return;
    }
    const float m1 = s1 / sumW;
    const float m2 = s2 / sumW;
    const float v = m2 - m1 * m1;
    const float boost = F.varianceBoost / (len > 1.0 ? len : 1.0);
    dst[i] = float4(sr / sumW, sg / sumW, sb / sumW, (v > 0.0 ? v : 0.0) * boost);
}
