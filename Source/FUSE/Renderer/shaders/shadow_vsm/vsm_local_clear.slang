// WP-3.2 "vsm.local_clear". Slang twin of vsm_local_clear.comp (see there).
#include "vsm_raster_common.slang"

[numthreads(16, 16, 1)]
void main(uint3 group: SV_GroupID, uint3 lid: SV_GroupThreadID) {
    VsmShadow* S = fuseVsmrShadow();
    const uint page = fuse_vsmr_words[fuseVsmrIndex(S.workHandle)][kFuseVsmrWordLocalList + group.x * 2u + 1u];
    const uint2 origin = uint2((page % S.localPagesX) * 128u, (page / S.localPagesX) * 128u) + lid.xy * 8u;
    const uint image = fuseVsmrIndex(S.localPool);
    const uint value = S.localClear;
    for (uint y = 0u; y < 8u; ++y) {
        for (uint x = 0u; x < 8u; ++x) {
            fuse_vsmr_images[image][origin + uint2(x, y)] = value;
        }
    }
}
