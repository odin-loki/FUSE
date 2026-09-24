// WP-5.1 task (amplification) shader, Slang twin of meshlet.task (see there).
#include "gpu_scene.slang"
#include "cull_common.slang"
#include "meshlet_common.slang"

[[vk::binding(3, 0)]] RWStructuredBuffer<uint> fuse_meshlet_words[];

struct Push {
    float4 viewProj[4];
    uint scene;
    uint target64;
    uint width;
    uint height;
    uint64_t constants;
    uint mode;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

groupshared MeshletPayload payload;
groupshared uint s_draw[32];
groupshared uint s_defer[32];

[shader("amplification")]
[numthreads(32, 1, 1)]
void main(uint3 localId: SV_GroupThreadID, uint3 groupId: SV_GroupID) {
    CullConstants* C = (CullConstants*)pc.constants;
    MeshletTail* M = (MeshletTail*)(pc.constants + kFuseMeshletTailOffset);
    const uint lane = localId.x;
    const uint g = groupId.x;
    const uint mode = pc.mode;
    const uint region = mode == kFuseMeshletModePhase2 ? 1u : 0u;
    const uint groups = fuseHandleIndex(M.groupsBuffer);
    const uint masks = fuseHandleIndex(M.masksBuffer);
    const uint record = region * M.groupCapacity + g;
    const uint instance = fuse_meshlet_words[groups][record * 2u];
    const uint first = fuse_meshlet_words[groups][record * 2u + 1u];
    GpuSceneHeader* scene = fuseGpuScene(pc.scene);
    const GpuInstance inst = fuseGpuSceneInstances(scene)[instance];
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    const uint m = first + lane;
    uint result = kFuseMeshletResultNone;
    bool write = mode != kFuseMeshletModeLate;
    if (m < mesh.meshletCount) {
        const GpuMeshlet ml = ((GpuMeshlet*)mesh.meshlets)[m];
        const GpuTransform xf = fuseGpuSceneTransforms(scene)[instance];
        if (mode == kFuseMeshletModeLate) {
            if (((fuse_meshlet_words[masks][g] >> lane) & 1u) != 0u) {
                result = fuseMeshletClassifyLate(C, xf, ml);
                write = true;
            }
        } else {
            const GpuTransform prevXf = fuseGpuScenePrevTransforms(scene)[instance];
            result = fuseMeshletClassify(C, M, mode, xf, prevXf, ml);
            if (result == kFuseMeshletResultOversize) {
                uint previous;
                InterlockedAdd(fuse_meshlet_words[fuseHandleIndex(M.countsBuffer)][kFuseMeshletCountOversize], 1u, previous);
            }
        }
    }
    if (write && (M.flags & kFuseMeshletWriteResults) != 0u) {
        fuse_meshlet_words[fuseHandleIndex(M.resultsBuffer)][record * kFuseMeshletTaskGroup + lane] = result;
    }
    const bool draw = result == kFuseMeshletResultPhase1Drawn || result == kFuseMeshletResultPhase2Drawn;
    s_draw[lane] = draw ? 1u : 0u;
    s_defer[lane] = result == kFuseMeshletResultDeferred ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    uint before = 0u;
    uint total = 0u;
    uint deferMask = 0u;
    for (uint j = 0u; j < kFuseMeshletTaskGroup; ++j) {
        before += j < lane ? s_draw[j] : 0u;
        total += s_draw[j];
        deferMask |= s_defer[j] << j;
    }
    if (draw) {
        payload.meshlets[before] = m;
    }
    if (lane == 0u) {
        payload.instance = instance;
        if (mode == kFuseMeshletModeEarly) {
            fuse_meshlet_words[masks][g] = deferMask;
        }
    }
    DispatchMesh(total, 1u, 1u, payload);
}
