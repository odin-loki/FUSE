// FUSE Relight RL-5.2 "relight.restir_di.*": the ReSTIR DI stages (restir_di_core.h) on the path tracer's scene
// (rl_pt.slang: the WP-6.0 TLAS by ray query, tables by device address, RL-4.4 lights, RL-4.3 BSDF). One pipeline,
// the stage in the push constants: 0 presample (x < tileSize, y < tileCount), 1 surface, 2 initial, 3 temporal,
// 4 shade, 8 + i spatial iteration i. CPU oracle: RestirDiCpu (src/restir_di.cpp). GLSL twin: restir_di.comp.
struct RdiPush {
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
    uint64_t restirDi; ///< 0 (the apply hook is the trace pass's)
    uint64_t block;    ///< RdiBlock: 8 addresses + kRdiParamWords float4
    uint64_t src;      ///< the stage's input reservoirs
    uint64_t dst;      ///< the stage's output
    uint width;
    uint height;
    uint lightCount;
    uint stage;
};
[[vk::push_constant]] ConstantBuffer<RdiPush> pc;

#include "rl_pt.slang"

// RdiBlock addresses: 0 surfaces (this frame), 1 surfaces (previous), 2 history reservoirs, 3 tiles, 4 pmf, 5 cdf,
// 6 output; the params at byte 64.
uint64_t rdiBlock(uint k) { return ((uint64_t*)pc.block)[k]; }

float4 rdiSurfaceWord(uint slot, uint pixel, uint k) {
    return ((float4*)rdiBlock(slot == 0u ? 0u : 1u))[pixel * 4u + k];
}
float4 rdiReservoirWord(uint which, uint pixel, uint k) {
    uint64_t a = which == 0u ? pc.src : rdiBlock(2u);
    return ((float4*)a)[pixel * 2u + k];
}
uint rdiTileLight(uint entry) { return ((uint*)rdiBlock(3u))[entry]; }
float rdiTilePmf(uint light) { return ((float*)rdiBlock(4u))[light]; }
float rdiCdf(uint light) { return ((float*)rdiBlock(5u))[light]; }
float rdiTreePmf(float3 p, float3 n, uint light) { return ltPmf(pc.lightTree, p, n, light); }

#define RDI_CTX_PARAM
#define RDI_CTX_ARG
#define RDI_PT
#define RDI_LUT
#define RDI_PARAM_WORDS(name) float4 name[4]

#include "restir_di_core.h"

void rdiStoreReservoir(uint pixel, RdiReservoir r) {
    ((float4*)pc.dst)[pixel * 2u] = rdiReservoirWord0(r);
    ((float4*)pc.dst)[pixel * 2u + 1u] = rdiReservoirWord1(r);
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
    if (pc.stage == 0u) {
        uint entry = tid.y * pc.width + tid.x;
        ((uint*)pc.dst)[entry] = rdiPresample(P, entry);
        return;
    }
    float4 rw[4];
    for (uint k = 0u; k < 4u; ++k) {
        rw[k] = ((float4*)(pc.block + 64))[k];
    }
    RdiParams R = rdiParamsUnpack(rw);
    uint pixel = tid.y * P.width + tid.x;
    if (pc.stage == 1u) {
        PtParams Q = P;
        Q.flags = (P.flags | kPtFlagDiRecord) & ~(kPtFlagNee | kPtFlagRestirDi);
        g_rdiHave = false;
        PtSample s = ptRenderSample(Q, tid.x, tid.y, P.sampleBase);
        float4 w0;
        float4 w1;
        float4 w2;
        float4 w3;
        rdiSurfaceWords(P, g_rdiHave, g_rdiHit, g_rdiDir, w0, w1, w2, w3);
        float4* o = (float4*)pc.dst;
        o[pixel * 4u] = w0;
        o[pixel * 4u + 1u] = w1;
        o[pixel * 4u + 2u] = w2;
        o[pixel * 4u + 3u] = w3;
        return;
    }
    if (pc.stage == 4u) {
        RdiReservoir r = rdiLoadReservoir(0u, pixel);
        float4* o = (float4*)rdiBlock(6u);
        o[pixel * 2u] = rdiSurfaceWord(0u, pixel, 0u);
        o[pixel * 2u + 1u] = rdiShade(P, r, pixel);
        float4* hst = (float4*)rdiBlock(2u);
        hst[pixel * 2u] = rdiReservoirWord0(r);
        hst[pixel * 2u + 1u] = rdiReservoirWord1(r);
        return;
    }
    if (pc.stage == 2u) {
        rdiStoreReservoir(pixel, rdiInitial(P, R, tid.x, tid.y));
        return;
    }
    if (pc.stage >= 8u) {
        R.iteration = pc.stage - 8u;
    }
    rdiStoreReservoir(pixel, rdiReuse(P, R, tid.x, tid.y, pc.stage == 3u));
}
