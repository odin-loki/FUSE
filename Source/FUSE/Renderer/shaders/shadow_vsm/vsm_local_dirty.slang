// WP-3.2 "vsm.local_dirty". Slang twin of vsm_local_dirty.comp (see there).
#include "vsm_raster_common.slang"

bool fuseVsmrRecordTouches(uint* b, uint at, VsmLocal e) {
    if (b[at + 3u] == 0u) {
        return false;
    }
    const float3 c = float3(asfloat(b[at]), asfloat(b[at + 1u]), asfloat(b[at + 2u]));
    const float3 x = float3(asfloat(b[at + 4u]), asfloat(b[at + 5u]), asfloat(b[at + 6u])) * 1.01;
    return fuseVsmrBoxSphere(c, x, float3(e.position[0], e.position[1], e.position[2]), e.range * 1.01);
}

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    VsmShadow* S = fuseVsmrShadow();
    VsmConstants* C = (VsmConstants*)S.vsm;
    const uint i = tid.x;
    if (i >= C.instanceCount || i >= C.boundsCapacity) {
        return;
    }
    uint* b = (uint*)C.bounds;
    const uint prev = (C.boundsRead + i) * 8u;
    const uint cur = (C.boundsWrite + i) * 8u;
    bool same = true;
    for (uint k = 0u; k < 8u; ++k) {
        same = same && b[prev + k] == b[cur + k];
    }
    if (same) {
        return;
    }
    uint mask = 0u;
    const uint count = min(S.localCount, kFuseVsmrMaxLocal);
    for (uint k = 0u; k < count; ++k) {
        const VsmLocal e = S.local[k];
        if (fuseVsmrRecordTouches(b, prev, e) || fuseVsmrRecordTouches(b, cur, e)) {
            mask |= 1u << k;
        }
    }
    if (mask != 0u) {
        uint previous;
        InterlockedOr(fuse_vsmr_words[fuseVsmrIndex(S.workHandle)][kFuseVsmrWordDirty], mask, previous);
    }
}
