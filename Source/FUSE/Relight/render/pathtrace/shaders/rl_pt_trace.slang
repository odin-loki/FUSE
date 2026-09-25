// FUSE Relight RL-5.1 "relight.pt.trace". Slang twin of rl_pt_trace.comp (see there).
struct PtTracePush {
    uint64_t params;
    uint64_t instances;
    uint64_t triangles;
    uint64_t materials;
    uint64_t portals;
    uint64_t lightMap;
    uint64_t lightTable;
    uint64_t lightTree;
    uint64_t tlas;
    uint64_t lut;
    uint64_t outputs;
    uint64_t outStride;
    uint width;
    uint height;
    uint accumulate;
    uint lightCount;
    uint64_t restirDi; ///< RL-5.2 ReSTIR DI output (RestirDiGpu), 0: off
};
[[vk::push_constant]] ConstantBuffer<PtTracePush> pc;

#include "rl_pt.slang"

uint64_t ptSection(uint s) { return pc.outputs + uint64_t(s) * pc.outStride; }

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    uint2 pix = tid.xy;
    if (pix.x >= pc.width || pix.y >= pc.height) {
        return;
    }
    float4 w[11];
    for (uint k = 0u; k < 11u; ++k) {
        w[k] = ((float4*)pc.params)[k];
    }
    PtParams P = ptParamsUnpack(w);
    uint i = pix.y * pc.width + pix.x;
    float3 sum = float3(0.0, 0.0, 0.0);
    float3 sumSq = float3(0.0, 0.0, 0.0);
    float3 em = float3(0.0, 0.0, 0.0);
    float3 di = float3(0.0, 0.0, 0.0);
    float3 sp = float3(0.0, 0.0, 0.0);
    PtSample g = ptRenderSample(P, pix.x, pix.y, P.sampleBase);
    uint spp = max(P.samplesPerPixel, 1u);
    for (uint s = 0u; s < spp; ++s) {
        PtSample r = s == 0u ? g : ptRenderSample(P, pix.x, pix.y, P.sampleBase + s);
        sum += r.radiance;
        sumSq += r.radiance * r.radiance;
        em += r.emissive;
        di += r.diffuse;
        sp += r.specular;
    }
    float inv = 1.0 / float(spp);
    ((float4*)ptSection(0u))[i] = float4(sum * inv, float(spp));
    float4 acc = float4(sum, float(spp));
    float4 accSq = float4(sumSq, 0.0);
    if (pc.accumulate != 0u) {
        acc += ((float4*)ptSection(1u))[i];
        accSq += ((float4*)ptSection(2u))[i];
    }
    ((float4*)ptSection(1u))[i] = acc;
    ((float4*)ptSection(2u))[i] = accSq;
    ((float4*)ptSection(3u))[i] = float4(em * inv, 0.0);
    bool specHit = (g.flags & kPtSampleSpecularHit) != 0u;
    ((float4*)ptSection(4u))[i] = float4(di * inv, specHit ? 0.0 : g.hitDist);
    ((float4*)ptSection(5u))[i] = float4(sp * inv, specHit ? g.hitDist : 0.0);
    ((float4*)ptSection(6u))[i] = float4(g.albedoD, float(g.psr));
    ((float4*)ptSection(7u))[i] = float4(g.albedoS, float(g.flags));
    ((float4*)ptSection(8u))[i] = float4(g.normal, g.roughness);
    ((float2*)ptSection(9u))[i] = float2(g.motionX, g.motionY);
    ((float*)ptSection(10u))[i] = g.depth;
    ((uint*)ptSection(11u))[i] = g.instance;
}
