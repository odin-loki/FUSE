// WP-6.4 A-SVGF gradients ("denoise.gradient.prepare" / ".atrous"). Slang twin of dn_gradient.comp (see there).
#include "dn_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const DnFrame F = dnFrame();
    const uint2 sp = tid.xy;
    if (sp.x >= F.strataW || sp.y >= F.strataH) {
        return;
    }
    const uint s = sp.y * F.strataW + sp.x;
    const uint pi = dnStratumPixel(F, sp.x, sp.y);
    float4* guide = dnTexels(F.guideCur);
    float4* src = dnTexels(pc.src);
    float4* dst = dnTexels(pc.dst);
    const float4 g = guide[pi];
    const float4 inp = src[s];
    if (pc.mode == kDnGradientPrepare) {
        if (!(inp.z > 0.0) || !(g.w > 0.0)) {
            dst[s] = float4(0.0, 0.0, 0.0, 0.0);
            return;
        }
        const float a = abs(inp.x);
        const float b = abs(inp.y);
        dst[s] = float4(inp.x - inp.y, a > b ? a : b, 1.0, 0.0);
        return;
    }
    if (!(g.w > 0.0)) {
        dst[s] = inp;
        return;
    }
    float sumW = 0.0;
    float sd = 0.0;
    float sm = 0.0;
    float sv = 0.0;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int qx = int(sp.x) + dx * int(pc.step);
            const int qy = int(sp.y) + dy * int(pc.step);
            if (qx < 0 || qy < 0 || qx >= int(F.strataW) || qy >= int(F.strataH)) {
                continue;
            }
            const uint q = uint(qy) * F.strataW + uint(qx);
            const uint qi = dnStratumPixel(F, uint(qx), uint(qy));
            const float4 gq = guide[qi];
            if (!(gq.w > 0.0)) {
                continue;
            }
            const float h = dnAtrousTap(dx) * dnAtrousTap(dy);
            const float wz = abs(g.w - gq.w) / (F.sigmaDepth * g.w * F.reprojDepth + F.depthEpsilon);
            const float wn = dnNormalWeight(g, gq, F.sigmaNormal);
            const float wgt = h * wn * exp(-wz);
            const float4 v = src[q];
            sumW = sumW + wgt;
            sd = sd + wgt * v.x;
            sm = sm + wgt * v.y;
            sv = sv + wgt * v.z;
        }
    }
    dst[s] = float4(sd / sumW, sm / sumW, sv / sumW, 0.0);
}
