// FUSE Relight RL-4.3 GPU gate kernel (rl_bsdf_vk_parity_slang): runs bsdfEval, bsdfPdf and bsdfSample of
// bsdf.slang over a list of cases, one per thread; the CPU twin is the bsdf_eval / bsdf_pdf / bsdf_sample kernels
// (Relight/kernels/bsdf_kernels.hpp). GLSL twin: bsdf_probe.comp.
//
// Case layout (kProbeCaseWords = 14 float4): 11 packed material rows, wo, wi, u.
// Output (3 float4 per case): (eval.xyz, pdf), (sample.wi.xyz, sample.pdf), (sample.weight.xyz, float(flags)).
[[vk::binding(0, 0)]] StructuredBuffer<float4> g_cases;
[[vk::binding(1, 0)]] StructuredBuffer<float> g_bsdfLut;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> g_out;

#define FUSE_BSDF_LUT_READ(i) g_bsdfLut[(i)]
#include "bsdf.slang"

struct FuseBsdfProbePush {
    uint count;
    uint pad0;
    uint pad1;
    uint pad2;
};
[[vk::push_constant]] ConstantBuffer<FuseBsdfProbePush> pc;

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= pc.count) {
        return;
    }
    uint base = i * 14u;
    float4 w[11];
    for (uint k = 0u; k < 11u; ++k) {
        w[k] = g_cases[base + k];
    }
    BsdfMaterial m = bsdfMaterialUnpack(w);
    float3 wo = g_cases[base + 11u].xyz;
    float3 wi = g_cases[base + 12u].xyz;
    float4 u = g_cases[base + 13u];
    float3 e = bsdfEval(m, wo, wi);
    float pdf = bsdfPdf(m, wo, wi);
    BsdfSample s = bsdfSample(m, wo, u);
    g_out[i * 3u + 0u] = float4(e, pdf);
    g_out[i * 3u + 1u] = float4(s.wi, s.pdf);
    g_out[i * 3u + 2u] = float4(s.weight, float(s.flags));
}
