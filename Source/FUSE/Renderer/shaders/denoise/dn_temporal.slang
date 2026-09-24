// WP-6.4 "denoise.temporal". Slang twin of dn_temporal.comp (see there).
#include "dn_common.slang"

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
    const float4 sig = dnReadSignal(F, i);
    const float l = dnLuminance(sig, F.flags);
    const float4 g = dnTexels(F.guideCur)[i];
    float4* accum = dnTexels(F.accum);
    float4* momCur = dnTexels(F.momCur);
    if (!(g.w > 0.0)) {
        accum[i] = sig;
        momCur[i] = float4(l, l * l, 0.0, 0.0);
        return;
    }
    float sumW = 0.0;
    float hr = 0.0;
    float hg = 0.0;
    float hb = 0.0;
    float m1 = 0.0;
    float m2 = 0.0;
    float len = 0.0;
    if ((F.flags & kDnFlagHistory) != 0u) {
        const float2 m = dnPairs(F.motion)[i];
        float px = (float(p.x) + 0.5) - m.x * float(w);
        float py = (float(p.y) + 0.5) - m.y * float(h);
        px = px - 0.5;
        py = py - 0.5;
        px = px > -2.0 ? (px < float(w) + 1.0 ? px : float(w) + 1.0) : -2.0;
        py = py > -2.0 ? (py < float(h) + 1.0 ? py : float(h) + 1.0) : -2.0;
        const float x0 = floor(px);
        const float y0 = floor(py);
        const float fx = px - x0;
        const float fy = py - y0;
        const int ix = int(x0);
        const int iy = int(y0);
        const float2 gz = dnPairs(F.gradZ)[i];
        const float tol = F.reprojDepth * g.w + (abs(gz.x) + abs(gz.y));
        float4* guidePrev = dnTexels(F.guidePrev);
        float4* histPrev = dnTexels(F.histPrev);
        float4* momPrev = dnTexels(F.momPrev);
        for (uint t = 0u; t < 4u; ++t) {
            const int tx = ix + int(t & 1u);
            const int ty = iy + int(t >> 1u);
            const float wx = (t & 1u) != 0u ? fx : 1.0 - fx;
            const float wy = (t >> 1u) != 0u ? fy : 1.0 - fy;
            const float wt = wx * wy;
            if (tx < 0 || ty < 0 || tx >= int(w) || ty >= int(h) || !(wt > 0.0)) {
                continue;
            }
            const uint j = uint(ty) * w + uint(tx);
            const float4 gp = guidePrev[j];
            if (!(gp.w > 0.0) || abs(gp.w - g.w) > tol || dnDot3(gp, g) < F.reprojNormal) {
                continue;
            }
            const float4 hc = histPrev[j];
            const float4 hm = momPrev[j];
            sumW = sumW + wt;
            hr = hr + wt * hc.x;
            hg = hg + wt * hc.y;
            hb = hb + wt * hc.z;
            m1 = m1 + wt * hm.x;
            m2 = m2 + wt * hm.y;
            len = len + wt * hm.z;
        }
    }
    const bool valid = sumW >= F.minReprojWeight && sumW > 0.0;
    float aC = 1.0;
    float aM = 1.0;
    float newLen = 1.0;
    if (valid) {
        hr = hr / sumW;
        hg = hg / sumW;
        hb = hb / sumW;
        m1 = m1 / sumW;
        m2 = m2 / sumW;
        len = len / sumW;
        newLen = len + 1.0;
        newLen = newLen < F.maxHistory ? newLen : F.maxHistory;
        float lambda = 0.0;
        if ((F.flags & kDnFlagGradients) != 0u) {
            const uint si = (p.y / kDnStratum) * F.strataW + p.x / kDnStratum;
            lambda = dnGradientLambda(F, dnTexels(F.lambda)[si]);
            newLen = newLen * (1.0 - lambda);
            newLen = newLen > 1.0 ? newLen : 1.0;
        }
        const float inv = 1.0 / newLen;
        aC = F.alphaColor > inv ? F.alphaColor : inv;
        aM = F.alphaMoments > inv ? F.alphaMoments : inv;
        if ((F.flags & kDnFlagGradients) != 0u) {
            aC = aC * (1.0 - lambda) + lambda;
            aM = aM * (1.0 - lambda) + lambda;
        }
    } else {
        hr = 0.0;
        hg = 0.0;
        hb = 0.0;
        m1 = 0.0;
        m2 = 0.0;
    }
    const float bC = 1.0 - aC;
    const float bM = 1.0 - aM;
    accum[i] = float4(hr * bC + sig.x * aC, hg * bC + sig.y * aC, hb * bC + sig.z * aC, 0.0);
    momCur[i] = float4(m1 * bM + l * aM, m2 * bM + (l * l) * aM, newLen, 0.0);
}
