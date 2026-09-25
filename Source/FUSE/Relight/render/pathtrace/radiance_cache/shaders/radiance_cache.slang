// FUSE Relight RL-5.4 "relight.radiance_cache.*": the hash-grid radiance cache stages (kernels/radiance_cache_core.h;
// training paths: the path tracer's core rl_pt.slang with kPtFlagRcTrain and the hook radiance_cache_hook.slang). One
// pipeline, the stage in the push constants: 0 clear (one slot per item; item 0 the header), 1 train (one training path
// per tile; item 0 writes the header: the frame's RcParams, zeroed counters), 2 update (one record per item), 3 resolve
// (one slot per item), 4 probe (gates: the answer at each record). 1D items over a 2D dispatch (index = y x pitch + x). CPU oracle: RadianceCacheCpu. GLSL twin:
// radiance_cache.comp.
struct RcPush {
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
    uint64_t restirDi; ///< 0 (rl_pt.slang's DI hook reads it)
    uint64_t block;    ///< RcBlock: table, records, probe addresses, pad, RcParams at byte 32
    uint width;
    uint height;
    uint lightCount;
    uint stage;
    uint count;        ///< items
    uint pitch;        ///< items per dispatch row (groupsX x 64)
};
[[vk::push_constant]] ConstantBuffer<RcPush> pc;

#include "rl_pt.slang"

void rcTrain(uint tile, RcParams R) {
    float4 w[11];
    for (uint k = 0u; k < 11u; ++k) {
        w[k] = ((float4*)pc.params)[k];
    }
    PtParams P = ptParamsUnpack(w);
    PtParams Q = P;
    Q.flags = (P.flags | kPtFlagRcTrain) &
              ~(kPtFlagRadianceCache | kPtFlagRestirDi | kPtFlagDiRecord | kPtFlagRestirGi | kPtFlagGiRecord);
    Q.rcTableLo = 0u;
    Q.rcTableHi = 0u;
    uint px;
    uint py;
    rcTrainPixel(R, P.width, P.height, tile, px, py);
    PtSample s = ptRenderSample(Q, px, py, P.sampleBase + 1073741824u);
}

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    uint i = tid.y * pc.pitch + tid.x;
    if (i >= pc.count) {
        return;
    }
    g_rcTable = ((uint64_t*)pc.block)[0];
    g_rcRecords = ((uint64_t*)pc.block)[1];
    g_rcParams = pc.block + 32;
    float4 hw[5];
    for (uint k = 0u; k < 5u; ++k) {
        hw[k] = ((float4*)g_rcParams)[k];
    }
    RcParams R = rcParamsUnpack(hw);
    if (pc.stage == 0u) {
        if (i == 0u) {
            rcWriteHeader(hw);
        }
        rcClearSlot(i);
        return;
    }
    if (pc.stage == 1u) {
        if (i == 0u) {
            rcWriteHeader(hw);
        }
        rcTrain(i, R);
        return;
    }
    if (pc.stage == 2u) {
        float4* rec = (float4*)g_rcRecords;
        rcUpdateRecord(R, rec[i * 3u], rec[i * 3u + 1u], rec[i * 3u + 2u]);
        return;
    }
    if (pc.stage == 4u) {
        float4* rec = (float4*)g_rcRecords;
        float4 a = rec[i * 3u];
        float4 o = float4(0.0, 0.0, 0.0, -1.0);
        if (a.w > 0.5) {
            float3 L;
            float samples;
            float4 nw = rec[i * 3u + 1u];
            if (rcLookup(R, float3(a.x, a.y, a.z), float3(nw.x, nw.y, nw.z), L, samples)) {
                o = float4(L.x, L.y, L.z, samples);
            }
        }
        ((float4*)(((uint64_t*)pc.block)[2]))[i] = o;
        return;
    }
    rcResolveSlot(R, i);
}
