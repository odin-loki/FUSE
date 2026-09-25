// FUSE Relight RL-4.4 "relight.lights.convert". Slang twin of rl_light_convert.comp (see there).
#include "rl_lights.slang"

struct RlConvertPush {
    uint64_t source;
    uint64_t table;
    uint count;
    uint leastSquares;
    float sphereRadius;
    float intensityFactor;
    float maxIntensity;
    float distantIntensity;
    float distantAngle;
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<RlConvertPush> pc;

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= pc.count) {
        return;
    }
    float4* src = (float4*)pc.source;
    float4* dst = (float4*)pc.table;
    float4 w[5];
    for (uint k = 0u; k < 5u; ++k) {
        w[k] = src[i * kRlD3dWords + k];
    }
    RlConvertParams prm;
    prm.sphereRadius = pc.sphereRadius;
    prm.intensityFactor = pc.intensityFactor;
    prm.maxIntensity = pc.maxIntensity;
    prm.distantIntensity = pc.distantIntensity;
    prm.distantAngle = pc.distantAngle;
    prm.leastSquares = pc.leastSquares;
    RlLight L = rlConvertD3dLight(rlD3dLightUnpack(w), prm);
    for (uint k = 0u; k < 6u; ++k) {
        dst[i * kRlLightWords + k] = rlLightWord(L, k);
    }
}
