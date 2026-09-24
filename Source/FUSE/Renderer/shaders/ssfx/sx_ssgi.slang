// WP-6.3 "ssfx.ssgi". Slang twin of sx_ssgi.comp (see there).
#include "sx_trace.slang"

static const float kSxGiTwoPi = 6.28318548;

float3 sxSampleDirection(float3 n, uint x, uint y, uint i, uint j, uint sampleSqrt) {
    const uint k = max(1u, sampleSqrt);
    const uint seed = (x * 73856093u) ^ (y * 19349663u) ^ ((i * k + j) * 83492791u);
    const float u1 = (float(i) + sxHashUnit(seed)) / float(k);
    const float u2 = (float(j) + sxHashUnit(seed ^ 0x9e3779b9u)) / float(k);
    const float3 helper = abs(n.x) < 0.9 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    const float3 tangent = sxNormalize(sxCross(helper, n));
    const float3 bitangent = sxCross(n, tangent);
    const float r = sqrt(u1);
    const float phi = kSxGiTwoPi * u2;
    const float lz = sqrt(max(0.0, 1.0 - u1));
    return sxAdd(sxAdd(sxScale(tangent, r * cos(phi)), sxScale(bitangent, r * sin(phi))), sxScale(n, lz));
}

float3 sxPixelGather(uint64_t radianceIn, uint x, uint y) {
    if (x >= F.width || y >= F.height || sxDepthAt(x, y) <= 0.0) {
        return float3(0.0);
    }
    SxTrace P;
    P.maxSteps = F.giMaxSteps;
    P.stride = F.giStride;
    P.thickness = F.giThickness;
    P.maxDistance = F.giMaxDistance;
    P.fadeEdge = 0.0;
    P.refineSteps = F.giRefineSteps;
    const float3 p = sxPositionAt(x, y);
    float3 n = sxNormalize(sxNormalAt(x, y));
    if (sxDot(n, p) > 0.0) {
        n = sxNeg(n);
    }
    float3 sum = float3(0.0);
    const uint total = F.giSampleSqrt * F.giSampleSqrt;
    for (uint i = 0u; i < F.giSampleSqrt; ++i) {
        for (uint j = 0u; j < F.giSampleSqrt; ++j) {
            const float3 dir = sxSampleDirection(n, x, y, i, j, F.giSampleSqrt);
            const SxHit hit = sxTraceRay(radianceIn, P, x, y, dir);
            if (!hit.hit) {
                continue;
            }
            const uint hx = min(F.width - 1u, uint(max(0.0, hit.px)));
            const uint hy = min(F.height - 1u, uint(max(0.0, hit.py)));
            float3 hitN = sxNormalize(sxNormalAt(hx, hy));
            if (sxDot(hitN, sxPositionAt(hx, hy)) > 0.0) {
                hitN = sxNeg(hitN);
            }
            if (sxDot(hitN, dir) >= 0.0) {
                continue;
            }
            sum = sxAdd(sum, hit.color);
        }
    }
    return sxScale(sum, 1.0 / float(total));
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    sxBegin();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const uint i = sxIndex(p.x, p.y);
    const float3 gathered = sxPixelGather(pc.src, p.x, p.y);
    const float3 a = sxTexels(F.diffuse)[i].xyz;
    const float3 indirect = sxScale(float3(a.x * gathered.x, a.y * gathered.y, a.z * gathered.z), F.giIntensity);
    sxTexels(F.gi)[i] = float4(indirect, 0.0);
    if (pc.dst != 0) {
        const float3 direct = sxTexels(F.radiance)[i].xyz;
        sxTexels(pc.dst)[i] = float4(sxAdd(direct, indirect), 0.0);
    }
}
