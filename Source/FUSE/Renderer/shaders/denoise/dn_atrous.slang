// WP-6.4 "denoise.atrous". Slang twin of dn_atrous.comp (see there).
#include "dn_common.slang"

[[vk::binding(0, 0)]] [format("rgba16f")] RWTexture2D<float4> fuse_dn_images[];

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const DnFrame F = dnFrame();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const uint w = F.width;
    const uint h = F.height;
    const uint i = p.y * w + p.x;
    float4* guide = dnTexels(F.guideCur);
    float4* src = dnTexels(pc.src);
    const float4 g = guide[i];
    const float4 inp = src[i];
    float4 outv = inp;
    if (g.w > 0.0) {
        float gs = 0.0;
        float gw = 0.0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int qx = int(p.x) + dx;
                const int qy = int(p.y) + dy;
                if (qx < 0 || qy < 0 || qx >= int(w) || qy >= int(h)) {
                    continue;
                }
                const uint q = uint(qy) * w + uint(qx);
                if (!(guide[q].w > 0.0)) {
                    continue;
                }
                const float k = dnGaussTap(dx) * dnGaussTap(dy);
                gs = gs + k * src[q].w;
                gw = gw + k;
            }
        }
        const float gvar = gs / gw;
        const float phiL = F.sigmaLuminance * sqrt(gvar > 0.0 ? gvar : 0.0) + 1e-6;
        const float2 gz = dnPairs(F.gradZ)[i];
        const float lp = dnLuminance(inp, F.flags);
        const int step = int(pc.step);
        float sumW = 0.0;
        float sr = 0.0;
        float sg = 0.0;
        float sb = 0.0;
        float sv = 0.0;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                const int ox = dx * step;
                const int oy = dy * step;
                const int qx = int(p.x) + ox;
                const int qy = int(p.y) + oy;
                if (qx < 0 || qy < 0 || qx >= int(w) || qy >= int(h)) {
                    continue;
                }
                const uint q = uint(qy) * w + uint(qx);
                const float4 gq = guide[q];
                if (!(gq.w > 0.0)) {
                    continue;
                }
                const float4 sq = src[q];
                const float k = dnAtrousTap(dx) * dnAtrousTap(dy);
                const float wz = dnDepthTerm(F, g.w, gq.w, gz, float(ox), float(oy));
                const float wn = dnNormalWeight(g, gq, F.sigmaNormal);
                const float wl = abs(lp - dnLuminance(sq, F.flags)) / phiL;
                const float wgt = k * wn * exp(-(wz + wl));
                sumW = sumW + wgt;
                sr = sr + wgt * sq.x;
                sg = sg + wgt * sq.y;
                sb = sb + wgt * sq.z;
                sv = sv + (wgt * wgt) * sq.w;
            }
        }
        if (sumW > 0.0) {
            outv = float4(sr / sumW, sg / sumW, sb / sumW, sv / (sumW * sumW));
        }
    }
    dnTexels(pc.dst)[i] = outv;
    if (pc.aux != 0ULL) {
        dnTexels(pc.aux)[i] = float4(outv.xyz, 0.0);
    }
    if ((pc.flags & kDnPassWriteImage) != 0u && F.outputImage != 0u) {
        const float4 img = (F.flags & kDnFlagScalar) != 0u ? float4(outv.x, outv.x, outv.x, outv.w) : outv;
        fuse_dn_images[F.outputImage & 0xFFFFFu][p] = img;
    }
}
