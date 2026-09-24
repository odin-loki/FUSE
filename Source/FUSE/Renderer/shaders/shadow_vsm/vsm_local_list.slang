// WP-3.2 "vsm.local_list". Slang twin of vsm_local_list.comp (see there).
#include "vsm_raster_common.slang"

[numthreads(1, 1, 1)]
void main() {
    VsmShadow* S = fuseVsmrShadow();
    const uint words = fuseVsmrIndex(S.workHandle);
    const uint mask = fuse_vsmr_words[words][kFuseVsmrWordDirty] | S.forceMask;
    uint n = 0u;
    const uint count = min(S.localCount, kFuseVsmrMaxLocal);
    for (uint k = 0u; k < count; ++k) {
        if (((mask >> k) & 1u) == 0u) {
            continue;
        }
        const uint faces = min(S.local[k].faces, 6u);
        for (uint f = 0u; f < faces && n < kFuseVsmrMaxLocalPages; ++f) {
            fuse_vsmr_words[words][kFuseVsmrWordLocalList + n * 2u] = k | (f << 8u);
            fuse_vsmr_words[words][kFuseVsmrWordLocalList + n * 2u + 1u] = S.local[k].page[f];
            ++n;
        }
    }
    fuse_vsmr_words[words][kFuseVsmrWordLocalX] = n;
    fuse_vsmr_words[words][kFuseVsmrWordLocalX + 1u] = 1u;
    fuse_vsmr_words[words][kFuseVsmrWordLocalX + 2u] = 1u;
}
