// "ssrc.gather". Slang twin of src_gather.comp (see there).
#include "src_common.slang"

[[vk::binding(0, 0)]] [format("rgba16f")] RWTexture2D<float4> fuse_src_images[];

float4 srcGather(uint x, uint y, uint index, out float4 composed) {
    const float4 g = srcTexels(F.geo)[index];
    const float4 l = srcTexels(F.lit)[index];
    float ir = 0.0;
    float ig = 0.0;
    float ib = 0.0;
    float ao = 1.0;
    float cr = l.x;
    float cg = l.y;
    float cb = l.z;
    if (g.x > 0.0) {
        const float a = srcRayCoord(x, F.cx, F.fx);
        const float b = srcRayCoord(y, F.cy, F.fy);
        float3 n = srcNormalize(g.yzw);
        if (srcDot(n, float3(a, b, 1.0)) > 0.0) {
            n = srcScale(n, -1.0);
        }
        const uint count = F.dirRes[0] * F.dirRes[0];
        float cw = 0.0;
        for (uint k = 0u; k < count; ++k) {
            cw = cw + srcBinWeight(k, n);
        }
        float4 w;
        const uint4 q = srcBilinear(float(x) + 0.5, float(y) + 0.5, F.spacing[0], F.probesX[0], F.probesY[0], w);
        float sr = 0.0;
        float sg = 0.0;
        float sb = 0.0;
        float sv = 0.0;
        float sw = 0.0;
        for (uint j = 0u; j < 4u; ++j) {
            const float wj = w[j];
            if (!(wj > 0.0)) {
                continue;
            }
            const uint ux = (j & 1u) != 0u ? q.y : q.x;
            const uint uy = (j >> 1u) != 0u ? q.w : q.z;
            const SrcProbe qp = srcProbeAt(0u, ux, uy);
            if (!qp.valid) {
                continue;
            }
            const uint base = (uy * F.probesX[0] + ux) * count;
            float pr = 0.0;
            float pg = 0.0;
            float pb = 0.0;
            float pv = 0.0;
            for (uint k = 0u; k < count; ++k) {
                const float wk = srcBinWeight(k, n);
                const float4 rec = srcDecode(srcRecords(pc.upper)[base + k]);
                pr = pr + wk * rec.x;
                pg = pg + wk * rec.y;
                pb = pb + wk * rec.z;
                pv = pv + wk * rec.w;
            }
            sr = sr + wj * pr;
            sg = sg + wj * pg;
            sb = sb + wj * pb;
            sv = sv + wj * pv;
            sw = sw + wj;
        }
        if (sw > 0.0 && cw > 0.0) {
            const float den = sw * cw;
            const float4 dif = srcTexels(F.diffuse)[index];
            const float4 alb = srcTexels(F.albedo)[index];
            ir = dif.x * (sr / den) * F.intensity;
            ig = dif.y * (sg / den) * F.intensity;
            ib = dif.z * (sb / den) * F.intensity;
            ao = sv / den;
            const float kk = dif.w * (ao - 1.0);
            cr = (l.x + ir) + F.ambient[0] * alb.x * kk;
            cg = (l.y + ig) + F.ambient[1] * alb.y * kk;
            cb = (l.z + ib) + F.ambient[2] * alb.z * kk;
        }
    }
    composed = float4(max(cr, 0.0), max(cg, 0.0), max(cb, 0.0), l.w);
    return float4(ir, ig, ib, ao);
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    srcBegin();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const uint index = p.y * F.width + p.x;
    float4 composed;
    const float4 indirect = srcGather(p.x, p.y, index, composed);
    srcTexels(F.indirect)[index] = indirect;
    const float4 result = (F.flags & kSrcFlagCompose) != 0u ? composed : indirect;
    fuse_src_images[F.output_ & 0xFFFFFu][p] = result;
    if (F.dump != 0) {
        srcTexels(F.dump)[index] = result;
    }
}
