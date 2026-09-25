// FUSE Relight RL-5.5 "relight.denoise.gradient": the A-SVGF gradient producer (rl_dn_gradient_core.h). Primary
// kernel (-fp-mode precise); GLSL twin: rl_dn_gradient.comp (see there).
struct RldnPush {
    uint64_t params; ///< this frame's kPtParamWords, then the previous frame's
    uint64_t instances;
    uint64_t triangles;
    uint64_t materials;
    uint64_t portals;
    uint64_t lightMap;
    uint64_t lightTable;
    uint64_t lightTree;
    uint64_t tlas;
    uint64_t lut;
    uint64_t restirDi; ///< 0: the ReSTIR hooks stay off
    uint64_t records;  ///< f32x4 per stratum (read, then rewritten)
    uint64_t gradient; ///< f32x4 per stratum (out)
    uint64_t reserved;
    uint strataW;
    uint strataH;
    uint lightCount;
    uint havePrev;
};
[[vk::push_constant]] ConstantBuffer<RldnPush> pc;

#include "rl_pt.slang"
#include "rl_dn_gradient_core.h"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.strataW || tid.y >= pc.strataH) {
        return;
    }
    float4 w[11];
    float4 pw[11];
    for (uint k = 0u; k < 11u; ++k) {
        w[k] = ((float4*)pc.params)[k];
        pw[k] = ((float4*)pc.params)[11u + k];
    }
    PtParams cur = ptParamsUnpack(w);
    PtParams prev = ptParamsUnpack(pw);
    uint s = tid.y * pc.strataW + tid.x;
    float4 rec = ((float4*)pc.records)[s];
    float4 grad;
    float4 next;
    rldnStratum(cur, prev, pc.havePrev != 0u, rec, tid.x, tid.y, grad, next);
    ((float4*)pc.gradient)[s] = grad;
    ((float4*)pc.records)[s] = next;
}
