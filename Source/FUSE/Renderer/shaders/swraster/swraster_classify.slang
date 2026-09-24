// WP-5.4 cluster classifier, Slang twin of swraster_classify.comp (see there).
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

[numthreads(32, 1, 1)]
void main(uint3 groupId: SV_GroupID, uint3 localId: SV_GroupThreadID) {
    SwConstants* C = (SwConstants*)pc.constants;
    const uint region = pc.region;
    const uint g = groupId.x;
    const uint lane = localId.x;
    const uint cap = C.capacity;
    if (g >= cap) {
        return;
    }
    const uint groups = fuseSwHandleIndex(C.groupsBuffer);
    const uint counts = fuseSwHandleIndex(C.countsBuffer);
    const uint r = (region * cap + g) * 2u;
    const uint instance = fuse_sw_words[groups][r];
    const uint m = fuse_sw_words[groups][r + 1u] + lane;
    GpuSceneHeader* scene = fuseGpuScene(C.scene);
    const GpuInstance inst = fuseGpuSceneInstances(scene)[instance];
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    uint result = kFuseSwResultNone;
    uint previous;
    if (m < mesh.meshletCount) {
        const GpuMeshlet ml = ((GpuMeshlet*)mesh.meshlets)[m];
        const GpuTransform xf = fuseGpuSceneTransforms(scene)[instance];
        result = fuseSwClassify(ml, xf, C);
        if (result == kFuseSwResultSoftware) {
            uint at;
            InterlockedAdd(fuse_sw_words[counts][kFuseSwCountSwRequested0 + region], 1u, at);
            if (at < cap) {
                const uint o = (region * cap + at) * 2u;
                fuse_sw_words[fuseSwHandleIndex(C.swBuffer)][o] = instance;
                fuse_sw_words[fuseSwHandleIndex(C.swBuffer)][o + 1u] = m;
                InterlockedMax(fuse_sw_words[counts][kFuseSwCountSoftware0 + region * 3u], at + 1u, previous);
            } else {
                InterlockedAdd(fuse_sw_words[counts][kFuseSwCountOverflow], 1u, previous);
            }
        } else if (result >= kFuseSwResultHardwareSize && result <= kFuseSwResultHardwareForced) {
            uint at;
            InterlockedAdd(fuse_sw_words[counts][kFuseSwCountHwRequested0 + region], 1u, at);
            if (at < cap) {
                const uint o = (region * cap + at) * 2u;
                fuse_sw_words[fuseSwHandleIndex(C.hwBuffer)][o] = instance;
                fuse_sw_words[fuseSwHandleIndex(C.hwBuffer)][o + 1u] = m;
                InterlockedMax(fuse_sw_words[counts][kFuseSwCountHardware0 + region * 4u + 1u], at + 1u, previous);
            } else {
                InterlockedAdd(fuse_sw_words[counts][kFuseSwCountOverflow], 1u, previous);
            }
        } else if (result == kFuseSwResultOversize) {
            InterlockedAdd(fuse_sw_words[counts][kFuseSwCountOversize], 1u, previous);
        }
    }
    if ((C.flags & kFuseSwFlagWriteResults) != 0u && C.resultsBuffer != 0u) {
        fuse_sw_words[fuseSwHandleIndex(C.resultsBuffer)][(region * cap + g) * kFuseSwGroupSize + lane] = result;
    }
}
