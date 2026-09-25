// WP-8.1 "fog.integrate". Slang twin of fog_integrate.comp (see there).
#include "fog_common.slang"

[[vk::push_constant]] ConstantBuffer<FogPush> pc;

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    FogFrame* R = (FogFrame*)pc.frame;
    const uint col = tid.x;
    if (col >= R.gridX * R.gridY) {
        return;
    }
    const uint x = col % R.gridX;
    const uint y = col / R.gridX;
    float4* src = (float4*)R.historyCur;
    float4* dst = (float4*)R.integrated;
    const float sx = (float(x) + 0.5) * R.invGridX;
    const float sy = (float(y) + 0.5) * R.invGridY;
    const float rx = (sx * 2.0 - 1.0) * R.tanX;
    const float ry = (1.0 - sy * 2.0) * R.tanY;
    const float scale = sqrt(rx * rx + ry * ry + 1.0);
    float ar = 0.0;
    float ag = 0.0;
    float ab = 0.0;
    float t = 1.0;
    for (uint z = 0u; z < R.gridZ; ++z) {
        const uint i = fogIndex(R, x, y, z);
        const float4 s = src[i];
        const float start = z == 0u ? 0.0 : R.sliceDepth[z];
        const float ds = (R.sliceDepth[z + 1u] - start) * scale;
        const float sigma = max(s.w, 0.0);
        const float tau = sigma * ds;
        const float stepT = exp(-sigma * ds);
        float k = 0.0;
        if (tau < kFogSeriesTau) {
            k = ds * (1.0 - tau * (0.5 - tau * (1.0 / 6.0 - tau * (1.0 / 24.0 - tau * (1.0 / 120.0 - tau * (1.0 / 720.0 - tau * (1.0 / 5040.0)))))));
        } else {
            k = (1.0 - stepT) / sigma;
        }
        ar = ar + t * (s.x * k);
        ag = ag + t * (s.y * k);
        ab = ab + t * (s.z * k);
        t = t * stepT;
        dst[i] = float4(ar, ag, ab, t);
    }
}
