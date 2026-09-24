// WP-3.1 "vsm.invalidate". Slang twin of vsm_invalidate.comp (see there).
#include "vsm_includes.slang"

void clearRect(VsmConstants* C, uint level, int4 rect) {
    const uint table = fuseVsmHandleIndex(C.pageTableHandle);
    const uint work = fuseVsmHandleIndex(C.workHandle);
    const uint base = level * kFuseVsmPagesPerLevel;
    for (int y = rect.y; y <= rect.w; ++y) {
        for (int x = rect.x; x <= rect.z; ++x) {
            const uint v = base + fuseVsmSlot(y) * kFuseVsmPagesPerAxis + fuseVsmSlot(x);
            uint before = 0u;
            InterlockedAnd(fuse_vsm_words[table][v], ~kFuseVsmPteCached, before);
            if ((before & kFuseVsmPteCached) != 0u) {
                InterlockedAdd(fuse_vsm_words[work][kFuseVsmCounterInvalidated], 1u);
            }
        }
    }
}

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    VsmConstants* C = fuseVsmConstants();
    const uint i = tid.x;
    const uint level = tid.y;
    if (i >= C.instanceCount || level >= C.levels) {
        return;
    }
    GpuSceneHeader* scene = fuseGpuScene(C.scene);
    const GpuInstance inst = fuseGpuSceneInstances(scene)[i];
    const bool meshValid = inst.mesh < scene.counts[kFuseGpuSceneMeshes];
    GpuMesh mesh = {};
    if (meshValid) {
        mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    }
    const VsmBounds cur = fuseVsmBoundsRecord(inst, meshValid, mesh, fuseGpuSceneTransforms(scene)[i]);
    const uint bounds = fuseVsmHandleIndex(C.boundsHandle);
    VsmBounds old;
    bool same = true;
    for (uint k = 0u; k < 8u; ++k) {
        old.w[k] = fuse_vsm_words[bounds][(C.boundsRead + i) * 8u + k];
        same = same && old.w[k] == cur.w[k];
    }
    if (level == 0u) {
        for (uint k = 0u; k < 8u; ++k) {
            fuse_vsm_words[bounds][(C.boundsWrite + i) * 8u + k] = cur.w[k];
        }
    }
    if (same) {
        return;
    }
    int4 rect;
    if (fuseVsmBoundsRect(C, old, level, rect)) {
        clearRect(C, level, rect);
    }
    if (fuseVsmBoundsRect(C, cur, level, rect)) {
        clearRect(C, level, rect);
    }
}
