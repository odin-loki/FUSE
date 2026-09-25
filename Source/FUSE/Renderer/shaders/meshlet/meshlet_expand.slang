// WP-5.1 expand, Slang twin of meshlet_expand.comp (see there).
#include "gpu_scene.slang"
#include "cull_common.slang"
#include "meshlet_common.slang"

[[vk::binding(3, 0)]] RWStructuredBuffer<uint> fuse_meshlet_words[];

struct Push {
    uint64_t constants;
    uint phase;
    uint maxDraws;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    CullConstants* C = (CullConstants*)pc.constants;
    MeshletTail* M = (MeshletTail*)(pc.constants + kFuseMeshletTailOffset);
    const uint i = tid.x;
    const uint region = pc.phase - 1u;
    const uint cullCounts = fuseHandleIndex(C.countsBuffer);
    const uint draws = fuse_meshlet_words[cullCounts][region == 0u ? kFuseCullCountPhase1Draws : kFuseCullCountPhase2Draws];
    if (i >= draws || i >= pc.maxDraws) {
        return;
    }
    const uint slot = (region == 0u ? 0u : C.phase2DrawBase) + i;
    const uint instance = fuse_meshlet_words[fuseHandleIndex(C.argsBuffer)][slot * 5u + 4u];
    GpuSceneHeader* scene = fuseGpuScene(M.scene);
    const GpuInstance inst = fuseGpuSceneInstances(scene)[instance];
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    const uint counts = fuseHandleIndex(M.countsBuffer);
    uint previous;
    if (mesh.meshletCount == 0u || mesh.meshlets == 0ull || mesh.meshletVertices == 0ull || mesh.meshletTriangles == 0ull ||
        mesh.positions == 0ull) {
        InterlockedAdd(fuse_meshlet_words[counts][kFuseMeshletCountSkipped], 1u, previous);
        return;
    }
    const uint n = (mesh.meshletCount + kFuseMeshletTaskGroup - 1u) / kFuseMeshletTaskGroup;
    uint base;
    InterlockedAdd(fuse_meshlet_words[counts][kFuseMeshletCountRequested0 + region], n, base);
    const uint cap = M.groupCapacity;
    const uint groups = fuseHandleIndex(M.groupsBuffer);
    for (uint k = 0u; k < n && base + k < cap; ++k) {
        const uint r = (region * cap + base + k) * 2u;
        fuse_meshlet_words[groups][r] = instance;
        fuse_meshlet_words[groups][r + 1u] = k * kFuseMeshletTaskGroup;
    }
    const uint end = base + n;
    InterlockedMax(fuse_meshlet_words[counts][region == 0u ? kFuseMeshletCountTasks0 : kFuseMeshletCountTasks1], min(end, cap),
                   previous);
    if (end > cap) {
        InterlockedAdd(fuse_meshlet_words[counts][kFuseMeshletCountOverflow], end - max(base, cap), previous);
    }
}
