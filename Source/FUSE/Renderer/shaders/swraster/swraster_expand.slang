// WP-5.4 expand, Slang twin of swraster_expand.comp (see there).
#include "gpu_scene.slang"
#include "vis_format.slang"
#include "vis_common.slang"
#include "swraster_common.slang"

[[vk::binding(3, 0)]] RWStructuredBuffer<uint> fuse_sw_words[];

struct Push {
    uint64_t constants;
    uint region;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    SwConstants* C = (SwConstants*)pc.constants;
    const uint i = tid.x;
    const uint region = pc.region;
    const uint cullCounts = fuseSwHandleIndex(C.cullCountsBuffer);
    const uint draws = fuse_sw_words[cullCounts][region == 0u ? kFuseSwCullPhase1Draws : kFuseSwCullPhase2Draws];
    if (i >= draws || i >= C.maxDraws) {
        return;
    }
    const uint slot = (region == 0u ? 0u : C.phase2DrawBase) + i;
    const uint instance = fuse_sw_words[fuseSwHandleIndex(C.cullArgsBuffer)][slot * 5u + 4u];
    GpuSceneHeader* scene = fuseGpuScene(C.scene);
    const GpuInstance inst = fuseGpuSceneInstances(scene)[instance];
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    const uint counts = fuseSwHandleIndex(C.countsBuffer);
    uint previous;
    if (mesh.meshletCount == 0u || mesh.meshlets == 0ull || mesh.meshletVertices == 0ull || mesh.meshletTriangles == 0ull ||
        mesh.positions == 0ull) {
        InterlockedAdd(fuse_sw_words[counts][kFuseSwCountSkipped], 1u, previous);
        return;
    }
    const uint n = (mesh.meshletCount + kFuseSwGroupSize - 1u) / kFuseSwGroupSize;
    uint base;
    InterlockedAdd(fuse_sw_words[counts][kFuseSwCountGroupsRequested0 + region], n, base);
    const uint cap = C.capacity;
    const uint groups = fuseSwHandleIndex(C.groupsBuffer);
    for (uint k = 0u; k < n && base + k < cap; ++k) {
        const uint r = (region * cap + base + k) * 2u;
        fuse_sw_words[groups][r] = instance;
        fuse_sw_words[groups][r + 1u] = k * kFuseSwGroupSize;
    }
    const uint end = base + n;
    InterlockedMax(fuse_sw_words[counts][kFuseSwCountClassify0 + region * 3u], min(end, cap), previous);
    if (end > cap) {
        InterlockedAdd(fuse_sw_words[counts][kFuseSwCountOverflow], end - max(base, cap), previous);
    }
}
