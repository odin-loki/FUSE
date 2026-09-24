// WP-3.1 "vsm.update". Slang twin of vsm_update.comp (see there).
#include "vsm_includes.slang"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    VsmConstants* C = fuseVsmConstants();
    const uint v = tid.x;
    if (v >= C.virtualPages) {
        return;
    }
    const uint level = v / kFuseVsmPagesPerLevel;
    const uint r = v % kFuseVsmPagesPerLevel;
    const uint sx = r % kFuseVsmPagesPerAxis;
    const uint sy = r / kFuseVsmPagesPerAxis;
    const VsmLevel L = C.level[level];
    const bool changed = (L.flags & 1u) != 0u || fuseVsmSlotToAbs(sx, L.prevOriginX) != fuseVsmSlotToAbs(sx, L.originX) ||
                         fuseVsmSlotToAbs(sy, L.prevOriginY) != fuseVsmSlotToAbs(sy, L.originY);
    const uint table = fuseVsmHandleIndex(C.pageTableHandle);
    const uint work = fuseVsmHandleIndex(C.workHandle);
    uint e = fuse_vsm_words[table][v];
    if (changed && (e & kFuseVsmPteCached) != 0u) {
        e &= ~kFuseVsmPteCached;
        fuse_vsm_words[table][v] = e;
        InterlockedAdd(fuse_vsm_words[work][kFuseVsmCounterScrolled], 1u);
    }
    const uint bit = 1u << (v % 32u);
    if ((fuse_vsm_words[work][C.offRequest + v / 32u] & bit) == 0u) {
        return;
    }
    InterlockedAdd(fuse_vsm_words[work][kFuseVsmCounterRequested], 1u);
    if ((e & kFuseVsmPteMapped) != 0u) {
        InterlockedAdd(fuse_vsm_words[work][kFuseVsmCounterAlreadyMapped], 1u);
        fuse_vsm_words[fuseVsmHandleIndex(C.physMetaHandle)][(e & kFuseVsmPtePhysMask) * 2u + 1u] = C.frame;
    } else {
        InterlockedOr(fuse_vsm_words[work][C.offNeed + v / 32u], bit);
        InterlockedAdd(fuse_vsm_words[work][kFuseVsmCounterNeeded], 1u);
    }
}
