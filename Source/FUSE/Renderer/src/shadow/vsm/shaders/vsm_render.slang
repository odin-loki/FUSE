// WP-3.1 "vsm.render". Slang twin of vsm_render.comp (see there).
#include "vsm_includes.slang"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    VsmConstants* C = fuseVsmConstants();
    const uint v = tid.x;
    if (v >= C.virtualPages) {
        return;
    }
    const uint work = fuseVsmHandleIndex(C.workHandle);
    if ((fuse_vsm_words[work][C.offRequest + v / 32u] & (1u << (v % 32u))) == 0u) {
        return;
    }
    const uint table = fuseVsmHandleIndex(C.pageTableHandle);
    const uint e = fuse_vsm_words[table][v];
    if ((e & kFuseVsmPteMapped) == 0u || (e & kFuseVsmPteCached) != 0u) {
        return;
    }
    uint slot = 0u;
    InterlockedAdd(fuse_vsm_words[work][kFuseVsmCounterRenderCount], 1u, slot);
    fuse_vsm_words[work][C.offRenderList + slot * 2u] = v;
    fuse_vsm_words[work][C.offRenderList + slot * 2u + 1u] = e & kFuseVsmPtePhysMask;
    fuse_vsm_words[table][v] = e | kFuseVsmPteCached;
}
