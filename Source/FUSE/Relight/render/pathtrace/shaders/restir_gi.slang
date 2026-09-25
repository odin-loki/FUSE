// FUSE Relight RL-5.3 "relight.restir_gi.*": the ReSTIR GI stages (restir_gi_core.h) on the path tracer's scene
// (rl_pt.slang: the WP-6.0 TLAS by ray query, tables by device address, RL-4.4 lights, RL-4.3 BSDF). One pipeline,
// the stage in the push constants: 1 surface, 2 initial, 3 temporal, 4 shade, 8 + i spatial iteration i. CPU oracle:
// RestirGiCpu (src/restir_gi.cpp). GLSL twin: restir_gi.comp.
struct RgiPush {
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
    uint64_t restirDi; ///< 0 (rl_pt.slang's DI hook reads it; the apply hooks are the trace pass's)
    uint64_t block;    ///< RgiBlock: 4 addresses + kRgiParamWords float4
    uint64_t src;      ///< the stage's input reservoirs
    uint64_t dst;      ///< the stage's output
    uint width;
    uint height;
    uint lightCount;
    uint stage;
};
[[vk::push_constant]] ConstantBuffer<RgiPush> pc;

#include "rl_pt.slang"

// RgiBlock addresses: 0 surfaces (this frame), 1 surfaces (previous), 2 history reservoirs, 3 output; the params at
// byte 32.
uint64_t rgiBlock(uint k) { return ((uint64_t*)pc.block)[k]; }

float4 rgiSurfaceWord(uint slot, uint pixel, uint k) {
    return ((float4*)rgiBlock(slot == 0u ? 0u : 1u))[pixel * 4u + k];
}
float4 rgiReservoirWord(uint which, uint pixel, uint k) {
    uint64_t a = which == 0u ? pc.src : rgiBlock(2u);
    return ((float4*)a)[pixel * 9u + k];
}

#define RGI_CTX_PARAM
#define RGI_CTX_ARG
#define RGI_PT
#define RGI_LUT
#define RGI_PARAM_WORDS(name) float4 name[2]

#include "restir_gi_core.h"

void rgiStoreReservoir(uint64_t base, uint pixel, RgiReservoir r) {
    for (uint k = 0u; k < kRgiReservoirWords; ++k) {
        ((float4*)base)[pixel * kRgiReservoirWords + k] = rgiReservoirPack(r, k);
    }
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.width || tid.y >= pc.height) {
        return;
    }
    float4 w[11];
    for (uint k = 0u; k < 11u; ++k) {
        w[k] = ((float4*)pc.params)[k];
    }
    PtParams P = ptParamsUnpack(w);
    float4 rw[2];
    for (uint k = 0u; k < 2u; ++k) {
        rw[k] = ((float4*)(pc.block + 32))[k];
    }
    RgiParams R = rgiParamsUnpack(rw);
    uint pixel = tid.y * P.width + tid.x;
    if (pc.stage == 1u) {
        PtParams Q = P;
        Q.flags = (P.flags | kPtFlagGiRecord) & ~(kPtFlagNee | kPtFlagRestirDi | kPtFlagDiRecord | kPtFlagRestirGi);
        g_rgiHave = false;
        PtSample s = ptRenderSample(Q, tid.x, tid.y, P.sampleBase);
        float4 w0;
        float4 w1;
        float4 w2;
        float4 w3;
        rgiSurfaceWords(P, g_rgiHave, g_rgiHit, g_rgiDir, g_rgiBounce, w0, w1, w2, w3);
        float4* o = (float4*)pc.dst;
        o[pixel * 4u] = w0;
        o[pixel * 4u + 1u] = w1;
        o[pixel * 4u + 2u] = w2;
        o[pixel * 4u + 3u] = w3;
        return;
    }
    if (pc.stage == 4u) {
        RgiReservoir r = rgiLoadReservoir(0u, pixel);
        float4* o = (float4*)rgiBlock(3u);
        o[pixel * 2u] = rgiSurfaceWord(0u, pixel, 0u);
        o[pixel * 2u + 1u] = rgiShade(P, r, pixel);
        rgiStoreReservoir(rgiBlock(2u), pixel, r);
        return;
    }
    if (pc.stage == 2u) {
        rgiStoreReservoir(pc.dst, pixel, rgiInitial(P, tid.x, tid.y));
        return;
    }
    if (pc.stage >= 8u) {
        R.iteration = pc.stage - 8u;
    }
    rgiStoreReservoir(pc.dst, pixel, rgiReuse(P, R, tid.x, tid.y, pc.stage == 3u));
}
