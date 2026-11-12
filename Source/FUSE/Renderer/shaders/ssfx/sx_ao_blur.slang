// "ssfx.ao_blur". Slang twin of sx_ao_blur.comp (see there) and of fuse::compute::screen_space_kernels::ao_blur.
#include "sx_common.slang"

float sxBlurWeight(float planeDelta, float normalSimilarity) {
    const float depthDelta = abs(0.0 - planeDelta);
    if (depthDelta > F.aoBlurDepth) {
        return 0.0;
    }
    if (normalSimilarity < F.aoBlurNormal) {
        return 0.0;
    }
    const float depthWeight = 1.0 - fuseDivRn(depthDelta, max(F.aoBlurDepth, 1e-6));
    const float normalWeight = fuseDivRn(normalSimilarity - F.aoBlurNormal, max(1.0 - F.aoBlurNormal, 1e-6));
    return sxClamp(depthWeight * normalWeight, 0.0, 1.0);
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    sxBegin();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    float* raw = sxFloats(F.ao);
    const uint ci = sxIndex(p.x, p.y);
    const float cz = sxDepthAt(p.x, p.y);
    if (cz <= 0.0) {
        sxFloats(F.aoBlurred)[ci] = raw[ci];
        return;
    }
    const float3 cp = sxPositionAt(p.x, p.y);
    const float3 cn = sxNormalize(sxNormalAt(p.x, p.y));
    float sum = raw[ci];
    float total = 1.0;
    const int w = int(F.width);
    const int h = int(F.height);
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int nx = int(p.x) + dx;
            const int ny = int(p.y) + dy;
            if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h) {
                continue;
            }
            const uint ni = sxIndex(uint(nx), uint(ny));
            if (sxDepthAt(uint(nx), uint(ny)) <= 0.0) {
                continue;
            }
            const float3 np = sxPositionAt(uint(nx), uint(ny));
            const float3 nn = sxNormalize(sxNormalAt(uint(nx), uint(ny)));
            const float planeDelta = fuseDivRn(abs(sxDot(cn, sxSub(np, cp))), cz);
            const float wgt = sxBlurWeight(planeDelta, sxDot(cn, nn));
            sum = sum + wgt * raw[ni];
            total = total + wgt;
        }
    }
    sxFloats(F.aoBlurred)[ci] = fuseDivRn(sum, total);
}
