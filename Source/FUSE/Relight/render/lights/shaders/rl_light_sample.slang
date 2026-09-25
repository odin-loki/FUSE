// FUSE Relight RL-4.4 "relight.lights.sample". Slang twin of rl_light_sample.comp (see there).
#include "rl_lights.slang"

struct RlSamplePush {
    uint64_t table;
    uint64_t tree;
    uint64_t queries;
    uint64_t results;
    uint count;
    uint lightCount;
    uint reserved0;
    uint reserved1;
};
[[vk::push_constant]] ConstantBuffer<RlSamplePush> pc;

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= pc.count) {
        return;
    }
    float4* qs = (float4*)pc.queries;
    float4* rs = (float4*)pc.results;
    float4 q0 = qs[i * kRlQueryWords + 0u];
    float4 q1 = qs[i * kRlQueryWords + 1u];
    float4 q2 = qs[i * kRlQueryWords + 2u];
    float4 q3 = qs[i * kRlQueryWords + 3u];
    float3 p = q0.xyz;
    float4 r0 = float4(0.0, 0.0, 0.0, -1.0);
    float4 r1 = float4(0.0);
    float4 r2 = float4(0.0);
    float4 r3 = float4(0.0);
    float4 r4 = float4(0.0);
    RlSetSample s = rlSetSample(pc.table, pc.lightCount, pc.tree, p, q1.xyz, q0.w, q1.w, q2.w);
    if (s.light != kLtInvalid) {
        r0 = float4(s.s.position, float(s.light));
        r1 = float4(s.s.wi, s.pdf);
        r2 = float4(s.s.radiance, s.pmf);
        r4 = float4(s.s.dist, float(s.s.flags), s.s.pdf, 0.0);
    }
    if (q3.x >= 0.0 && uint(q3.x) < pc.lightCount) {
        uint light = uint(q3.x);
        RlLight L = rlLoadLight(pc.table, light);
        float3 rad = rlLightEval(L, p, q2.xyz);
        float pmf = ltPmf(pc.tree, p, q1.xyz, light);
        r3 = float4(rad, pmf * rlLightPdf(L, p, q2.xyz));
    }
    uint base = i * kRlResultWords;
    rs[base + 0u] = r0;
    rs[base + 1u] = r1;
    rs[base + 2u] = r2;
    rs[base + 3u] = r3;
    rs[base + 4u] = r4;
}
